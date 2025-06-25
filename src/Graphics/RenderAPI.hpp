#pragma once

#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include <string>

using namespace irr;
using namespace core;

// Forward declarations
struct vertex;
class mesh;
class camera;

// Texture handle - opaque to the user
typedef unsigned int TextureHandle;
const TextureHandle INVALID_TEXTURE = 0;

// Window handle - opaque to the user (could be HWND on Windows, Window on X11, etc.)
typedef void* WindowHandle;

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
    vector3f color = vector3f(1.0f, 1.0f, 1.0f);
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
    virtual void clear(const vector3f& color = vector3f(0.2f, 0.3f, 0.8f)) = 0;

    // Camera and transforms
    virtual void setCamera(const camera& cam) = 0;
    virtual void pushMatrix() = 0;
    virtual void popMatrix() = 0;
    virtual void translate(const vector3f& pos) = 0;
    virtual void rotate(const matrix4f& rotation) = 0;
    virtual void multiplyMatrix(const matrix4f& matrix) = 0;

    // Texture management
    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) = 0;
    virtual void bindTexture(TextureHandle texture) = 0;
    virtual void unbindTexture() = 0;
    virtual void deleteTexture(TextureHandle texture) = 0;

    // Mesh rendering
    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) = 0;

    // State management
    virtual void setRenderState(const RenderState& state) = 0;
    virtual void enableLighting(bool enable) = 0;
    virtual void setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& position) = 0;

    // Utility
    virtual const char* getAPIName() const = 0;
};

// Factory function to create render API instances
enum class RenderAPIType
{
    OpenGL,
    // Future: Vulkan, DirectX, etc.
};

IRenderAPI* CreateRenderAPI(RenderAPIType type);