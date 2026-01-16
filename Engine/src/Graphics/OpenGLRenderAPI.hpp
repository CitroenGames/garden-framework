#pragma once

#include "RenderAPI.hpp"
#include "PostProcessing.hpp"
#include "Skybox.hpp"
#include <glad/glad.h>  // MUST be before any OpenGL headers
#include <SDL.h>

// No longer need GL/gl.h or GL/glu.h - GLAD provides everything
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stack>

// Use SDL's OpenGL context type for cross-platform support
typedef SDL_GLContext OpenGLContext;

// Forward declarations
class Shader;
class ShaderManager;

class OpenGLRenderAPI : public IRenderAPI
{
private:
    WindowHandle window_handle;
    OpenGLContext gl_context;
    int viewport_width;
    int viewport_height;
    float field_of_view;
    RenderState current_state;

    // Modern OpenGL - Matrix management (replaces matrix stack)
    glm::mat4 projection_matrix;
    glm::mat4 view_matrix;
    glm::mat4 current_model_matrix;
    std::stack<glm::mat4> model_matrix_stack;

    // Lighting state (for shader uniforms)
    glm::vec3 current_light_direction;
    glm::vec3 current_light_ambient;
    glm::vec3 current_light_diffuse;
    bool lighting_enabled;

    // Shader management
    ShaderManager* shader_manager;

    // Post Processing
    PostProcessing* post_processing;

    // Skybox
    Skybox* skybox;

    // Shadow Mapping
    GLuint shadowMapFBO;
    GLuint shadowMapTexture;
    const unsigned int SHADOW_WIDTH = 2048;
    const unsigned int SHADOW_HEIGHT = 2048;
    glm::mat4 lightSpaceMatrix;
    bool in_shadow_pass;

    // Optimization state tracking
    GLuint current_shader_id;
    GLuint current_bound_texture_0;
    RenderState current_gpu_state;
    bool global_uniforms_dirty;

    // Internal helper methods
    glm::mat4 convertToGLM(const matrix4f& m) const;
    Shader* getShaderForRenderState(const RenderState& state);
    static void GLAPIENTRY openglDebugCallback(GLenum source, GLenum type, GLuint id,
                                               GLenum severity, GLsizei length,
                                               const GLchar* message, const void* userParam);

    // Existing internal helper methods
    bool createOpenGLContext(WindowHandle window);
    void destroyOpenGLContext();
    void setupOpenGLDefaults();
    void applyRenderState(const RenderState& state);
    GLenum getGLCullMode(CullMode mode);
    void setupBlending(BlendMode mode);
    void setupDepthTesting(DepthTest test, bool write);

public:
    OpenGLRenderAPI();
    virtual ~OpenGLRenderAPI();

    // IRenderAPI implementation
    virtual bool initialize(WindowHandle window, int width, int height, float fov) override;
    virtual void shutdown() override;
    virtual void resize(int width, int height) override;

    virtual void beginFrame() override;
    virtual void endFrame() override;
    virtual void present() override;
    virtual void clear(const vector3f& color = vector3f(0.2f, 0.3f, 0.8f)) override;

    virtual void setCamera(const camera& cam) override;
    virtual void pushMatrix() override;
    virtual void popMatrix() override;
    virtual void translate(const vector3f& pos) override;
    virtual void rotate(const matrix4f& rotation) override;
    virtual void multiplyMatrix(const matrix4f& matrix) override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping overrides
    virtual void beginShadowPass(const vector3f& lightDir) override;
    virtual void endShadowPass() override;
    virtual void bindShadowMap(int textureUnit) override;
    virtual matrix4f getLightSpaceMatrix() override;

    virtual IGPUMesh* createMesh() override;

    virtual const char* getAPIName() const override { return "OpenGL"; }
};