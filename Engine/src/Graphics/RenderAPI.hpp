#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <string>

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

// Abstract rendering API interface
class IRenderAPI
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

    // Texture management
    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) = 0;
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

    // Utility
    virtual const char* getAPIName() const = 0;
};

// Factory function to create render API instances
enum class RenderAPIType
{
    OpenGL,
    Vulkan,
    Headless
};

IRenderAPI* CreateRenderAPI(RenderAPIType type);