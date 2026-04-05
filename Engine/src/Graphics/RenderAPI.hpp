#pragma once

#include "EngineExport.h"
#include "EngineGraphicsExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <cstdint>
#include <functional>

// Forward declaration for SDL
struct SDL_Window;

// Forward declarations
struct vertex;
class mesh;
class camera;
class IGPUMesh;

// Texture handle - opaque to the user
typedef unsigned int TextureHandle;
const TextureHandle INVALID_TEXTURE = 0;

// Window handle - now uses SDL_Window for cross-platform support
typedef SDL_Window* WindowHandle;

// Render states
enum class CullMode
{
    None,
    Back,
    Front
};

enum class BlendMode
{
    None,
    Alpha,
    Additive
};

enum class DepthTest
{
    None,
    Less,
    LessEqual
};

struct RenderState
{
    CullMode cull_mode = CullMode::Back;
    BlendMode blend_mode = BlendMode::None;
    DepthTest depth_test = DepthTest::LessEqual;
    bool depth_write = true;
    bool lighting = true;
    glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
};

// GPU light structures for point/spot light constant buffers
static const int MAX_LIGHTS = 16;

struct alignas(16) GPUPointLight {
    glm::vec3 position;    float range;
    glm::vec3 color;       float intensity;
    glm::vec3 attenuation; float _pad0;
};

struct alignas(16) GPUSpotLight {
    glm::vec3 position;    float range;
    glm::vec3 direction;   float intensity;
    glm::vec3 color;       float innerCutoff;
    glm::vec3 attenuation; float outerCutoff;
};

struct alignas(16) LightCBuffer {
    GPUPointLight pointLights[MAX_LIGHTS];
    GPUSpotLight  spotLights[MAX_LIGHTS];
    int numPointLights;
    int numSpotLights;
    float _pad[2];
    glm::vec3 cameraPos;
    float _pad2;
};

// Abstract rendering API interface
class ENGINE_API IRenderAPI
{
public:
    virtual ~IRenderAPI() = default;

    // Initialization and cleanup
    virtual bool initialize(WindowHandle window, int width, int height, float fov) = 0;
    virtual void shutdown() = 0;
    virtual void resize(int width, int height) = 0;

    // Frame management
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void present() = 0; // Present/swap buffers
    virtual void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) = 0;

    // Camera and transforms
    virtual void setCamera(const camera& cam) = 0;
    virtual void pushMatrix() = 0;
    virtual void popMatrix() = 0;
    virtual void translate(const glm::vec3& pos) = 0;
    virtual void rotate(const glm::mat4& rotation) = 0;
    virtual void multiplyMatrix(const glm::mat4& matrix) = 0;

    // Matrix getters (for frustum culling)
    virtual glm::mat4 getProjectionMatrix() const = 0;
    virtual glm::mat4 getViewMatrix() const = 0;

    // Texture management
    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) = 0;
    virtual TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                bool flip_vertically = false, bool generate_mipmaps = true) = 0;
    virtual void bindTexture(TextureHandle texture) = 0;
    virtual void unbindTexture() = 0;
    virtual void deleteTexture(TextureHandle texture) = 0;

    // Mesh rendering
    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) = 0;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) = 0;

    // State management
    virtual void setRenderState(const RenderState& state) = 0;
    virtual void enableLighting(bool enable) = 0;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) = 0;
    virtual void setPointAndSpotLights(const LightCBuffer& lights) { (void)lights; }

    virtual void renderSkybox() = 0;

    // Shadow Mapping (CSM - Cascaded Shadow Maps)
    virtual void beginShadowPass(const glm::vec3& lightDir) = 0;
    virtual void beginShadowPass(const glm::vec3& lightDir, const camera& cam) = 0;
    virtual void beginCascade(int cascadeIndex) = 0;
    virtual void endShadowPass() = 0;
    virtual void bindShadowMap(int textureUnit) = 0;
    virtual glm::mat4 getLightSpaceMatrix() = 0;
    virtual int getCascadeCount() const = 0;
    virtual const float* getCascadeSplitDistances() const = 0;
    virtual const glm::mat4* getLightSpaceMatrices() const = 0;

    // Resource Creation
    virtual IGPUMesh* createMesh() = 0;

    // Debug rendering (lines for debug visualization)
    // vertices are pairs of {pos, color} packed as vertex structs (normals used as color, uv ignored)
    virtual void renderDebugLines(const vertex* vertices, size_t vertex_count) { (void)vertices; (void)vertex_count; }

    // Depth prepass support
    virtual void beginDepthPrepass() {}
    virtual void endDepthPrepass() {}
    virtual void renderMeshDepthOnly(const mesh& m) { (void)m; }

    // Utility
    virtual const char* getAPIName() const = 0;

    // Graphics settings
    virtual void setFXAAEnabled(bool enabled) = 0;
    virtual bool isFXAAEnabled() const = 0;
    virtual void setShadowQuality(int quality) = 0;  // 0=Off, 1=Low(1024), 2=Medium(2048), 3=High(4096)
    virtual int getShadowQuality() const = 0;

    // Autorelease pool support (Metal needs ObjC temporaries drained each frame)
    // Default implementation just calls the function directly.
    virtual void executeWithAutoreleasePool(std::function<void()> fn) { fn(); }

    // Offscreen viewport rendering (for editor)
    // Finalize scene render to an offscreen viewport texture (applies FXAA if enabled)
    virtual void endSceneRender() {}
    // Get the rendered scene as an ImGui-compatible texture ID (cast to ImTextureID by caller)
    virtual uint64_t getViewportTextureID() { return 0; }
    // Resize the offscreen viewport render target
    virtual void setViewportSize(int width, int height) { (void)width; (void)height; }
    // Render ImGui draw data to the screen backbuffer
    virtual void renderUI() {}

    // Preview render target (for asset preview panel)
    virtual void beginPreviewFrame(int width, int height) { (void)width; (void)height; }
    virtual void endPreviewFrame() {}
    virtual uint64_t getPreviewTextureID() { return 0; }
    virtual void destroyPreviewTarget() {}

    // PIE viewport render targets (for multi-player Play-In-Editor).
    // These allow rendering full scenes to additional offscreen textures.
    // When a PIE viewport is active, beginFrame()/endSceneRender() render to it
    // instead of the main viewport, making render_scene_to_texture() work transparently.
    virtual int  createPIEViewport(int width, int height) { (void)width; (void)height; return -1; }
    virtual void destroyPIEViewport(int id) { (void)id; }
    virtual void destroyAllPIEViewports() {}
    virtual void setPIEViewportSize(int id, int width, int height) { (void)id; (void)width; (void)height; }
    // Set the active scene target. -1 = main viewport (default), 0+ = PIE viewport.
    // When set, the next beginFrame()/endSceneRender() cycle renders to this target.
    virtual void setActiveSceneTarget(int pie_viewport_id) { (void)pie_viewport_id; }
    virtual uint64_t getPIEViewportTextureID(int id) { (void)id; return 0; }
};

// Factory function to create render API instances
enum class RenderAPIType
{
    Vulkan,
    D3D11,
    Metal,
    Headless
};

#ifdef __APPLE__
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::Metal;
#elif defined(_WIN32)
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::D3D11;
#else
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::Vulkan;
#endif

ENGINE_GRAPHICS_API IRenderAPI* CreateRenderAPI(RenderAPIType type);