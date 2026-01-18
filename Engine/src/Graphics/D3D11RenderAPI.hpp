#pragma once

#include "RenderAPI.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stack>
#include <array>
#include <unordered_map>
#include <string>

// Use ComPtr for automatic COM reference counting
using Microsoft::WRL::ComPtr;

// Forward declarations
class D3D11Mesh;

// D3D11 Texture wrapper
struct D3D11Texture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width;
    uint32_t height;
};

// Constant buffer structures (16-byte aligned)
struct alignas(16) GlobalCBuffer
{
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrices[4];
    glm::vec4 cascadeSplits;          // x,y,z,w = splits 0-3
    glm::vec3 lightDir;
    float cascadeSplit4;              // 5th split distance
    glm::vec3 lightAmbient;
    int cascadeCount;
    glm::vec3 lightDiffuse;
    int debugCascades;
};

struct alignas(16) PerObjectCBuffer
{
    glm::mat4 model;
    glm::vec3 color;
    int useTexture;
};

struct alignas(16) ShadowCBuffer
{
    glm::mat4 lightSpaceMatrix;
    glm::mat4 model;
};

struct alignas(16) SkyboxCBuffer
{
    glm::mat4 projection;
    glm::mat4 view;
    glm::vec3 sunDirection;
    float time;
};

struct alignas(16) FXAACBuffer
{
    glm::vec2 inverseScreenSize;
    glm::vec2 padding;
};

class D3D11RenderAPI : public IRenderAPI
{
private:
    WindowHandle window_handle;
    HWND hwnd;
    int viewport_width;
    int viewport_height;
    float field_of_view;
    RenderState current_state;

    // Core D3D11 objects
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTargetView;
    ComPtr<ID3D11Texture2D> depthStencilBuffer;
    ComPtr<ID3D11DepthStencilView> depthStencilView;

    // Rasterizer states
    ComPtr<ID3D11RasterizerState> rasterizerCullBack;
    ComPtr<ID3D11RasterizerState> rasterizerCullFront;
    ComPtr<ID3D11RasterizerState> rasterizerCullNone;
    ComPtr<ID3D11RasterizerState> rasterizerShadow;

    // Blend states
    ComPtr<ID3D11BlendState> blendStateNone;
    ComPtr<ID3D11BlendState> blendStateAlpha;
    ComPtr<ID3D11BlendState> blendStateAdditive;

    // Depth stencil states
    ComPtr<ID3D11DepthStencilState> depthStateLess;
    ComPtr<ID3D11DepthStencilState> depthStateLessEqual;
    ComPtr<ID3D11DepthStencilState> depthStateNone;
    ComPtr<ID3D11DepthStencilState> depthStateReadOnly;

    // Samplers
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11SamplerState> shadowSampler;
    ComPtr<ID3D11SamplerState> pointSampler;

    // Shaders - Basic
    ComPtr<ID3D11VertexShader> basicVertexShader;
    ComPtr<ID3D11PixelShader> basicPixelShader;
    ComPtr<ID3D11InputLayout> basicInputLayout;

    // Shaders - Unlit
    ComPtr<ID3D11VertexShader> unlitVertexShader;
    ComPtr<ID3D11PixelShader> unlitPixelShader;

    // Shaders - Shadow
    ComPtr<ID3D11VertexShader> shadowVertexShader;
    ComPtr<ID3D11PixelShader> shadowPixelShader;

    // Shaders - Skybox
    ComPtr<ID3D11VertexShader> skyVertexShader;
    ComPtr<ID3D11PixelShader> skyPixelShader;
    ComPtr<ID3D11InputLayout> skyInputLayout;

    // Shaders - FXAA
    ComPtr<ID3D11VertexShader> fxaaVertexShader;
    ComPtr<ID3D11PixelShader> fxaaPixelShader;
    ComPtr<ID3D11InputLayout> fxaaInputLayout;

    // Constant buffers
    ComPtr<ID3D11Buffer> globalCBuffer;
    ComPtr<ID3D11Buffer> perObjectCBuffer;
    ComPtr<ID3D11Buffer> shadowCBuffer;
    ComPtr<ID3D11Buffer> skyboxCBuffer;
    ComPtr<ID3D11Buffer> fxaaCBuffer;

    // Matrix management
    glm::mat4 projection_matrix;
    glm::mat4 view_matrix;
    glm::mat4 current_model_matrix;
    std::stack<glm::mat4> model_matrix_stack;

    // Lighting state
    glm::vec3 current_light_direction;
    glm::vec3 current_light_ambient;
    glm::vec3 current_light_diffuse;
    bool lighting_enabled;

    // Shadow Mapping - CSM
    static const int NUM_CASCADES = 4;
    unsigned int currentShadowSize = 4096;  // Runtime configurable shadow resolution
    int shadowQuality = 3;  // 0=Off, 1=Low(1024), 2=Medium(2048), 3=High(4096)
    ComPtr<ID3D11Texture2D> shadowMapArray;
    ComPtr<ID3D11DepthStencilView> shadowDSVs[NUM_CASCADES];
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    glm::mat4 lightSpaceMatrix;
    glm::mat4 lightSpaceMatrices[NUM_CASCADES];
    float cascadeSplitDistances[NUM_CASCADES + 1];
    float cascadeSplitLambda = 0.5f;
    int currentCascade = 0;
    bool in_shadow_pass;
    bool debugCascades = false;

    // Post-processing (FXAA)
    ComPtr<ID3D11Texture2D> offscreenTexture;
    ComPtr<ID3D11RenderTargetView> offscreenRTV;
    ComPtr<ID3D11ShaderResourceView> offscreenSRV;
    ComPtr<ID3D11Buffer> fxaaQuadVB;
    bool fxaaEnabled = true;

    // Skybox
    ComPtr<ID3D11Buffer> skyboxVB;

    // Texture management
    std::unordered_map<TextureHandle, D3D11Texture> textures;
    TextureHandle nextTextureHandle = 1;
    TextureHandle currentBoundTexture = INVALID_TEXTURE;

    // Default white texture
    TextureHandle defaultTexture = INVALID_TEXTURE;

    // Internal helper methods
    bool createDevice();
    bool createSwapChain();
    bool createRenderTargetView();
    bool createDepthStencilBuffer(int width, int height);
    bool createRasterizerStates();
    bool createBlendStates();
    bool createDepthStencilStates();
    bool createSamplers();
    bool loadShaders();
    bool createConstantBuffers();
    bool createShadowMapResources();
    bool createPostProcessingResources(int width, int height);
    bool createSkyboxResources();
    bool createDefaultTexture();

    ComPtr<ID3DBlob> compileShader(const std::string& source, const std::string& entryPoint,
                                    const std::string& target, const std::string& filename = "shader");
    bool loadShaderFromFile(const std::string& filepath, std::string& outSource);

    void applyRenderState(const RenderState& state);
    void updateGlobalCBuffer();
    void updatePerObjectCBuffer(const glm::vec3& color, bool useTexture);
    void updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model);

    // CSM helper methods
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                             const glm::mat4& viewMatrix, float fov, float aspect);
    void recreateShadowMapResources(unsigned int size);

public:
    D3D11RenderAPI();
    virtual ~D3D11RenderAPI();

    // IRenderAPI implementation
    virtual bool initialize(WindowHandle window, int width, int height, float fov) override;
    virtual void shutdown() override;
    virtual void resize(int width, int height) override;

    virtual void beginFrame() override;
    virtual void endFrame() override;
    virtual void present() override;
    virtual void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) override;

    virtual void setCamera(const camera& cam) override;
    virtual void pushMatrix() override;
    virtual void popMatrix() override;
    virtual void translate(const glm::vec3& pos) override;
    virtual void rotate(const glm::mat4& rotation) override;
    virtual void multiplyMatrix(const glm::mat4& matrix) override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                bool flip_vertically = false, bool generate_mipmaps = true) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping (CSM)
    virtual void beginShadowPass(const glm::vec3& lightDir) override;
    virtual void beginShadowPass(const glm::vec3& lightDir, const camera& cam) override;
    virtual void beginCascade(int cascadeIndex) override;
    virtual void endShadowPass() override;
    virtual void bindShadowMap(int textureUnit) override;
    virtual glm::mat4 getLightSpaceMatrix() override;
    virtual int getCascadeCount() const override;
    virtual const float* getCascadeSplitDistances() const override;
    virtual const glm::mat4* getLightSpaceMatrices() const override;

    virtual IGPUMesh* createMesh() override;

    virtual const char* getAPIName() const override { return "D3D11"; }

    // Graphics settings
    virtual void setFXAAEnabled(bool enabled) override;
    virtual bool isFXAAEnabled() const override;
    virtual void setShadowQuality(int quality) override;
    virtual int getShadowQuality() const override;

    // D3D11 specific accessors (for ImGui integration)
    ID3D11Device* getDevice() const { return device.Get(); }
    ID3D11DeviceContext* getDeviceContext() const { return context.Get(); }
};
