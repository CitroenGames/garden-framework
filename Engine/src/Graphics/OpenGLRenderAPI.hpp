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
#include <array>

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

    // Shadow Mapping - CSM (Cascaded Shadow Maps)
    static const int NUM_CASCADES = 4;
    GLuint shadowMapFBO;
    GLuint shadowMapTextureArray;  // GL_TEXTURE_2D_ARRAY for cascades
    const unsigned int SHADOW_WIDTH = 4096;
    const unsigned int SHADOW_HEIGHT = 4096;
    glm::mat4 lightSpaceMatrix;  // Keep for backwards compatibility
    glm::mat4 lightSpaceMatrices[NUM_CASCADES];
    float cascadeSplitDistances[NUM_CASCADES + 1];
    float cascadeSplitLambda = 0.5f;
    int currentCascade = 0;
    bool in_shadow_pass;
    bool debugCascades = false;

    // CSM helper methods
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
        const glm::mat4& viewMatrix, float fov, float aspect);

    // Optimization state tracking
    GLuint current_shader_id;
    GLuint current_bound_texture_0;
    RenderState current_gpu_state;
    bool global_uniforms_dirty;

    // Internal helper methods
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
    virtual void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) override;

    virtual void setCamera(const camera& cam) override;
    virtual void pushMatrix() override;
    virtual void popMatrix() override;
    virtual void translate(const glm::vec3& pos) override;
    virtual void rotate(const glm::mat4& rotation) override;
    virtual void multiplyMatrix(const glm::mat4& matrix) override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping overrides (CSM)
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

    virtual const char* getAPIName() const override { return "OpenGL"; }
};