#pragma once

#include "Graphics/RenderAPI.hpp"
#include "D3D12Types.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
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
#include <vector>
#include <mutex>

using Microsoft::WRL::ComPtr;

// Forward declarations
class D3D12Mesh;

class D3D12RenderAPI : public IRenderAPI
{
public:
    static const int NUM_BACK_BUFFERS = 2;
    static const int NUM_FRAMES_IN_FLIGHT = 2;

private:
    WindowHandle window_handle = nullptr;
    HWND hwnd = nullptr;
    int viewport_width = 0;
    int viewport_height = 0;
    float field_of_view = 75.0f;
    RenderState current_state;

    // Core D3D12 objects
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<IDXGIFactory4> dxgiFactory;

    // Per-frame resources
    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        UINT64 fenceValue = 0;
    };
    FrameContext m_frameContexts[NUM_FRAMES_IN_FLIGHT];
    UINT m_frameIndex = 0;
    UINT m_backBufferIndex = 0;

    // Command list (single, reused each frame)
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Fence for CPU/GPU synchronization
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;

    // Back buffer resources
    ComPtr<ID3D12Resource> m_backBuffers[NUM_BACK_BUFFERS];

    // Depth stencil
    ComPtr<ID3D12Resource> m_depthStencilBuffer;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap; // Shader-visible CBV_SRV_UAV

    DescriptorHeapAllocator m_rtvAllocator;
    DescriptorHeapAllocator m_dsvAllocator;
    DescriptorHeapAllocator m_srvAllocator;

    // RTV indices for back buffers
    UINT m_backBufferRTVs[NUM_BACK_BUFFERS] = {};

    // DSV index for main depth buffer
    UINT m_mainDSVIndex = UINT(-1);

    // Root signature
    ComPtr<ID3D12RootSignature> m_rootSignature;

    // Pipeline State Objects
    ComPtr<ID3D12PipelineState> m_psoBasicLit;
    ComPtr<ID3D12PipelineState> m_psoBasicLitCullFront;
    ComPtr<ID3D12PipelineState> m_psoBasicLitCullNone;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAlpha;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAlphaCullNone;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAdditive;
    ComPtr<ID3D12PipelineState> m_psoUnlit;
    ComPtr<ID3D12PipelineState> m_psoUnlitCullNone;
    ComPtr<ID3D12PipelineState> m_psoUnlitAlpha;
    ComPtr<ID3D12PipelineState> m_psoUnlitAlphaCullNone;
    ComPtr<ID3D12PipelineState> m_psoUnlitAdditive;
    ComPtr<ID3D12PipelineState> m_psoShadow;
    ComPtr<ID3D12PipelineState> m_psoSky;
    ComPtr<ID3D12PipelineState> m_psoFXAA;
    ComPtr<ID3D12PipelineState> m_psoDepthPrepass;
    ComPtr<ID3D12PipelineState> m_psoDebugLines;

    // Shader bytecode (DXIL)
    std::vector<char> m_basicVS, m_basicPS;
    std::vector<char> m_unlitVS, m_unlitPS;
    std::vector<char> m_shadowVS, m_shadowPS;
    std::vector<char> m_skyVS, m_skyPS;
    std::vector<char> m_fxaaVS, m_fxaaPS;

    // Per-frame upload ring buffers for constant data
    UploadRingBuffer m_cbUploadBuffer[NUM_FRAMES_IN_FLIGHT];

    // Upload command infrastructure (for textures/meshes)
    ComPtr<ID3D12CommandAllocator> m_uploadCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_uploadCmdList;
    ComPtr<ID3D12Fence> m_uploadFence;
    HANDLE m_uploadFenceEvent = nullptr;
    UINT64 m_uploadFenceValue = 0;

    // Matrix management
    glm::mat4 projection_matrix = glm::mat4(1.0f);
    glm::mat4 view_matrix = glm::mat4(1.0f);
    glm::mat4 current_model_matrix = glm::mat4(1.0f);
    std::stack<glm::mat4> model_matrix_stack;

    // Lighting state
    glm::vec3 current_light_direction = glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 current_light_ambient = glm::vec3(0.2f, 0.2f, 0.2f);
    glm::vec3 current_light_diffuse = glm::vec3(0.8f, 0.8f, 0.8f);
    bool lighting_enabled = true;
    LightCBuffer current_lights{};

    // Shadow Mapping - CSM
    static const int NUM_CASCADES = 4;
    unsigned int currentShadowSize = 4096;
    int shadowQuality = 3;
    ComPtr<ID3D12Resource> m_shadowMapArray;
    UINT m_shadowDSVIndices[NUM_CASCADES] = {};
    UINT m_shadowSRVIndex = UINT(-1);
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    glm::mat4 lightSpaceMatrices[NUM_CASCADES];
    float cascadeSplitDistances[NUM_CASCADES + 1] = {};
    float cascadeSplitLambda = 0.92f;
    int currentCascade = 0;
    bool in_shadow_pass = false;
    bool debugCascades = false;

    // Post-processing (FXAA)
    ComPtr<ID3D12Resource> m_offscreenTexture;
    UINT m_offscreenRTVIndex = UINT(-1);
    UINT m_offscreenSRVIndex = UINT(-1);
    ComPtr<ID3D12Resource> m_fxaaQuadVB;
    D3D12_VERTEX_BUFFER_VIEW m_fxaaQuadVBV = {};
    bool fxaaEnabled = true;

    // Skybox
    ComPtr<ID3D12Resource> m_skyboxVB;
    D3D12_VERTEX_BUFFER_VIEW m_skyboxVBV = {};

    // Texture management
    std::unordered_map<TextureHandle, D3D12Texture> textures;
    TextureHandle nextTextureHandle = 1;
    TextureHandle currentBoundTexture = INVALID_TEXTURE;
    TextureHandle defaultTexture = INVALID_TEXTURE;

    // State tracking
    ID3D12PipelineState* last_bound_pso = nullptr;
    bool global_cbuffer_dirty = true;
    bool in_depth_prepass = false;

    // Command list lifecycle
    bool m_commandListOpen = false;
    void ensureCommandListOpen();

    // Resource state tracking
    D3D12_RESOURCE_STATES m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES m_backBufferState[NUM_BACK_BUFFERS] = { D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT };

    // Device lost flag
    bool device_lost = false;

    // Deferred shadow map recreation
    bool shadow_resources_dirty = false;
    unsigned int pending_shadow_size = 0;

    // VSync / present interval
    int presentInterval = 1;

    // Internal helper methods
    bool createDevice();
    bool createCommandQueue();
    bool createSwapChain();
    bool createDescriptorHeaps();
    bool createBackBufferRTVs();
    bool createDepthStencilBuffer(int width, int height);
    bool createFrameResources();
    bool createFence();
    bool createUploadInfrastructure();
    bool createRootSignature();
    bool loadShaders();
    bool createPipelineStates();
    bool createConstantBufferUploadHeaps();
    bool createShadowMapResources();
    bool createPostProcessingResources(int width, int height);
    bool createSkyboxResources();
    bool createDefaultTexture();

    void waitForFence(UINT64 fenceValue);
    void flushGPU();
    void executeUploadCommandList();

    std::vector<char> readShaderBinary(const std::string& filepath);

    void transitionResource(ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after);
    void bindDummyRootParams();

    ID3D12PipelineState* selectPSO(const RenderState& state, bool unlit);
    void updateGlobalCBuffer();
    void updatePerObjectCBuffer(const glm::vec3& color, bool useTexture);
    void updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model);

    // CSM helper methods
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                             const glm::mat4& viewMatrix, float fov, float aspect);
    void recreateShadowMapResources(unsigned int size);

    // Helper to create a GPU buffer from CPU data
    ComPtr<ID3D12Resource> createBufferFromData(const void* data, size_t dataSize,
                                                 D3D12_RESOURCE_STATES finalState);

    // CPU-side mipmap generation
    std::vector<uint8_t> generateMipLevel(const uint8_t* src, int srcWidth, int srcHeight, int channels,
                                           int& outWidth, int& outHeight);

public:
    D3D12RenderAPI();
    virtual ~D3D12RenderAPI();

    // IRenderAPI implementation
    bool initialize(WindowHandle window, int width, int height, float fov) override;
    void shutdown() override;
    void waitForGPU() override;
    void resize(int width, int height) override;

    void beginFrame() override;
    void endFrame() override;
    void present() override;
    void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) override;

    void setCamera(const camera& cam) override;
    void pushMatrix() override;
    void popMatrix() override;
    void translate(const glm::vec3& pos) override;
    void rotate(const glm::mat4& rotation) override;
    void multiplyMatrix(const glm::mat4& matrix) override;

    glm::mat4 getProjectionMatrix() const override;
    glm::mat4 getViewMatrix() const override;

    TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                        bool flip_vertically = false, bool generate_mipmaps = true) override;
    void bindTexture(TextureHandle texture) override;
    void unbindTexture() override;
    void deleteTexture(TextureHandle texture) override;

    void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    void setRenderState(const RenderState& state) override;
    void enableLighting(bool enable) override;
    void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;
    void setPointAndSpotLights(const LightCBuffer& lights) override;

    void renderSkybox() override;

    // Shadow Mapping (CSM)
    void beginShadowPass(const glm::vec3& lightDir) override;
    void beginShadowPass(const glm::vec3& lightDir, const camera& cam) override;
    void beginCascade(int cascadeIndex) override;
    void endShadowPass() override;
    void bindShadowMap(int textureUnit) override;
    glm::mat4 getLightSpaceMatrix() override;
    int getCascadeCount() const override;
    const float* getCascadeSplitDistances() const override;
    const glm::mat4* getLightSpaceMatrices() const override;

    // Depth prepass
    void beginDepthPrepass() override;
    void endDepthPrepass() override;
    void renderMeshDepthOnly(const mesh& m) override;

    IGPUMesh* createMesh() override;

    // Debug line rendering
    void renderDebugLines(const vertex* vertices, size_t vertex_count) override;

    const char* getAPIName() const override { return "D3D12"; }

    // Graphics settings
    void setFXAAEnabled(bool enabled) override;
    bool isFXAAEnabled() const override;
    void setShadowQuality(int quality) override;
    int getShadowQuality() const override;

    // Viewport rendering (for editor)
    void endSceneRender() override;
    uint64_t getViewportTextureID() override;
    void setViewportSize(int width, int height) override;
    void renderUI() override;

    // Preview render target (for asset preview panel)
    void beginPreviewFrame(int width, int height) override;
    void endPreviewFrame() override;
    uint64_t getPreviewTextureID() override;
    void destroyPreviewTarget() override;

    // PIE viewport render targets
    int  createPIEViewport(int width, int height) override;
    void destroyPIEViewport(int id) override;
    void destroyAllPIEViewports() override;
    void setPIEViewportSize(int id, int width, int height) override;
    void setActiveSceneTarget(int pie_viewport_id) override;
    uint64_t getPIEViewportTextureID(int id) override;

    // D3D12 specific accessors (for ImGui integration)
    ID3D12Device* getDevice() const { return device.Get(); }
    ID3D12CommandQueue* getCommandQueue() const { return commandQueue.Get(); }
    ID3D12GraphicsCommandList* getCommandList() const { return commandList.Get(); }
    ID3D12DescriptorHeap* getSrvDescriptorHeap() const { return m_srvHeap.Get(); }
    DescriptorHeapAllocator& getSrvAllocator() { return m_srvAllocator; }

private:
    // Viewport render target for editor
    ComPtr<ID3D12Resource> m_viewportTexture;
    ComPtr<ID3D12Resource> m_viewportDepthBuffer;
    UINT m_viewportRTVIndex = UINT(-1);
    UINT m_viewportSRVIndex = UINT(-1);
    UINT m_viewportDSVIndex = UINT(-1);
    int viewport_width_rt = 0, viewport_height_rt = 0;
    void createViewportResources(int w, int h);

    // Preview render target for asset preview panel
    ComPtr<ID3D12Resource> m_previewTexture;
    ComPtr<ID3D12Resource> m_previewDepthBuffer;
    UINT m_previewRTVIndex = UINT(-1);
    UINT m_previewSRVIndex = UINT(-1);
    UINT m_previewDSVIndex = UINT(-1);
    int preview_width_rt = 0, preview_height_rt = 0;
    void createPreviewResources(int w, int h);

    // PIE viewport render targets
    struct PIEViewportTarget
    {
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Resource> depthBuffer;
        ComPtr<ID3D12Resource> offscreenTexture;
        UINT rtvIndex = UINT(-1);
        UINT srvIndex = UINT(-1);
        UINT dsvIndex = UINT(-1);
        UINT offscreenRTVIndex = UINT(-1);
        UINT offscreenSRVIndex = UINT(-1);
        int width = 0, height = 0;
    };
    std::unordered_map<int, PIEViewportTarget> m_pie_viewports;
    int m_next_pie_id = 0;
    int m_active_scene_target = -1;
    void createPIEViewportResources(PIEViewportTarget& target, int w, int h);
};
