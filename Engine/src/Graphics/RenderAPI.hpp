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
#include <vector>
#include <utility>
#include <functional>
#include <algorithm>
#include <memory>

#include "SceneViewport.hpp"

// Forward declaration for command buffer
class RenderCommandBuffer;

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
    bool alpha_test = false;
    float alpha_cutoff = 0.0f;
};

// GPU light structures for point/spot light constant buffers.
// MAX_LIGHTS is the forward-path CB cap. The deferred path uses a larger
// StructuredBuffer (see MAX_LIGHTS_DEFERRED on backends like D3D12RenderAPI).
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

// Shared backend light-cbuffer struct. On D3D12 forward (post-Phase-7) the
// shader only reads counts + cameraPos; per-light data comes from the
// StructuredBuffers populated via uploadLightBuffers. The arrays stay for
// Vulkan's forward path which still uses inline arrays.
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
    virtual void waitForGPU() {}
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
    // Load pre-compressed texture with pre-generated mip chain (BC1/BC3/BC5/BC7)
    // format: 0=RGBA8, 1=BC1, 2=BC3, 3=BC5, 4=BC7
    virtual TextureHandle loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                const std::vector<const uint8_t*>& mip_data,
                                                const std::vector<size_t>& mip_sizes,
                                                const std::vector<std::pair<int,int>>& mip_dimensions)
    { (void)width; (void)height; (void)format; (void)mip_count; (void)mip_data; (void)mip_sizes; (void)mip_dimensions; return INVALID_TEXTURE; }
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

    // Command buffer replay (for multicore rendering)
    // Replays a pre-recorded command buffer of self-contained draw commands.
    // Commands must be recorded with pre-computed model matrices and textures.
    virtual void replayCommandBuffer(const RenderCommandBuffer& cmds) { (void)cmds; }

    // Parallel replay: splits command buffer across multiple GPU command lists
    // recorded on worker threads. Falls back to single-threaded replay for
    // small buffers or if the backend doesn't support parallel replay.
    virtual void replayCommandBufferParallel(const RenderCommandBuffer& cmds) { replayCommandBuffer(cmds); }

    // Deferred rendering controls. `isDeferredEnabled` reflects the desired
    // toggle (matches the r_deferred CVar). `isDeferredActive` reports whether
    // it is currently in effect (desired AND backend resources initialized).
    // UI should bind to Enabled; the render path reads Active.
    virtual void setDeferredEnabled(bool enabled) { (void)enabled; }
    virtual bool isDeferredEnabled() const { return false; }
    virtual bool isDeferredActive() const { return false; }
    virtual void submitDeferredOpaqueCommands(const RenderCommandBuffer& cmds) { (void)cmds; }
    virtual void submitDeferredTransparentCommands(const RenderCommandBuffer& cmds) { (void)cmds; }

    // Populate the deferred path's light StructuredBuffers. Supports up to a
    // backend-specific cap (256 per kind on D3D12). The forward CB path is
    // independent and still populated via setPointAndSpotLights.
    virtual void uploadLightBuffers(const GPUPointLight* pts, int ptCount,
                                    const GPUSpotLight* spts, int spCount) {
        (void)pts; (void)ptCount; (void)spts; (void)spCount;
    }

    // Utility
    virtual const char* getAPIName() const = 0;

    // Graphics settings
    virtual void setFXAAEnabled(bool enabled) = 0;
    virtual bool isFXAAEnabled() const = 0;
    virtual void setShadowQuality(int quality) = 0;  // 0=Off, 1=Low(1024), 2=Medium(2048), 3=High(4096)
    virtual int getShadowQuality() const = 0;
    virtual void setSSAOEnabled(bool enabled) { (void)enabled; }
    virtual bool isSSAOEnabled() const { return false; }
    virtual void setSSAORadius(float radius) { (void)radius; }
    virtual void setSSAOIntensity(float intensity) { (void)intensity; }

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

    // Caller-owned scene viewports. Backends that haven't migrated yet return
    // nullptr from createSceneViewport — callers must fall back to the legacy
    // setViewportSize / setActiveSceneTarget API when that happens.
    //
    //   auto vp = api->createSceneViewport(w, h);
    //   api->setEditorViewport(vp.get());
    //   ...render...
    //   api->setEditorViewport(nullptr);  // before vp destruction or before API shutdown
    virtual std::unique_ptr<SceneViewport> createSceneViewport(int width, int height)
    {
        (void)width; (void)height;
        return nullptr;
    }
    virtual void setEditorViewport(SceneViewport* viewport) { (void)viewport; }

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
    D3D12,
    Metal,
    Headless
};

#ifdef __APPLE__
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::Metal;
#elif defined(_WIN32)
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::D3D12;
#else
constexpr RenderAPIType DefaultRenderAPI = RenderAPIType::Vulkan;
#endif

// String conversion utilities for RenderAPIType
inline const char* RenderAPITypeToString(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::Vulkan:   return "vulkan";
    case RenderAPIType::D3D12:    return "d3d12";
    case RenderAPIType::Metal:    return "metal";
    case RenderAPIType::Headless: return "headless";
    }
    return "vulkan";
}

inline RenderAPIType ParseRenderAPIType(const std::string& str)
{
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "vulkan")   return RenderAPIType::Vulkan;
    if (lower == "d3d12" || lower == "dx12" || lower == "direct3d12")
        return RenderAPIType::D3D12;
    if (lower == "metal")    return RenderAPIType::Metal;
    if (lower == "headless") return RenderAPIType::Headless;

    return DefaultRenderAPI;
}

inline bool IsRenderAPIPlatformAvailable(RenderAPIType type)
{
    switch (type)
    {
#ifdef _WIN32
    case RenderAPIType::D3D12:  return true;
#endif
#ifdef __APPLE__
    case RenderAPIType::Metal:  return true;
#endif
    case RenderAPIType::Vulkan:   return true;
    case RenderAPIType::Headless: return true;
    default: return false;
    }
}

ENGINE_GRAPHICS_API IRenderAPI* CreateRenderAPI(RenderAPIType type);