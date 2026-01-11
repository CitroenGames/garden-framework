#include "OpenGLRenderAPI.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/ShaderManager.hpp"
#include "Graphics/GPUMesh.hpp"
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

OpenGLRenderAPI::OpenGLRenderAPI()
    : window_handle(nullptr), gl_context(nullptr), viewport_width(0), viewport_height(0), field_of_view(75.0f),
      projection_matrix(1.0f), view_matrix(1.0f), current_model_matrix(1.0f),
      current_light_position(1.0f, 1.0f, 1.0f), current_light_ambient(0.2f, 0.2f, 0.2f),
      current_light_diffuse(0.8f, 0.8f, 0.8f), lighting_enabled(true), shader_manager(nullptr)
{
}

OpenGLRenderAPI::~OpenGLRenderAPI()
{
    shutdown();
}

bool OpenGLRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    window_handle = window;
    viewport_width = width;
    viewport_height = height;
    field_of_view = fov;

    if (!createOpenGLContext(window))
    {
        printf("Failed to create OpenGL context\n");
        return false;
    }

    // Create shader manager and load default shaders
    shader_manager = new ShaderManager();
    shader_manager->createDefaultShaders();

    setupOpenGLDefaults();
    resize(width, height);

    printf("OpenGL Render API initialized (%dx%d, FOV: %.1f)\n", width, height, fov);
    return true;
}

void OpenGLRenderAPI::shutdown()
{
    // Clean up shader manager
    if (shader_manager)
    {
        delete shader_manager;
        shader_manager = nullptr;
    }

    // Note: No need to disable GL capabilities during shutdown
    // The context is about to be destroyed anyway

    destroyOpenGLContext();
}

void OpenGLRenderAPI::resize(int width, int height)
{
    viewport_width = width;
    viewport_height = height;

    float ratio = (float)width / (float)height;

    // Set up projection matrix using GLM
    projection_matrix = glm::perspective(
        glm::radians(field_of_view),
        ratio,
        0.1f,
        200.0f
    );

    // Set up viewport
    glViewport(0, 0, width, height);
}

bool OpenGLRenderAPI::createOpenGLContext(WindowHandle window)
{
    // Note: SDL_GL attributes are now set in Application::initialize()
    // before window creation, which is the proper SDL order.
    // This ensures attributes that affect window creation are set beforehand.

    // Create OpenGL context using SDL
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
    {
        printf("Failed to create OpenGL 4.6 Core context: %s\n", SDL_GetError());
        return false;
    }

    // Make the context current
    if (SDL_GL_MakeCurrent(window, gl_context) != 0)
    {
        printf("Failed to make OpenGL context current: %s\n", SDL_GetError());
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
        return false;
    }

    // Load all OpenGL 4.6 function pointers with GLAD
    if (!gladLoadGL())
    {
        printf("Failed to load OpenGL functions with GLAD\n");
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
        return false;
    }

    // Print OpenGL version information
    printf("OpenGL Version: %s\n", glGetString(GL_VERSION));
    printf("OpenGL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GLSL Version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

#ifdef _DEBUG
    // Enable OpenGL debug output in debug builds
    if (glDebugMessageCallback)
    {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(openglDebugCallback, nullptr);
        printf("OpenGL debug output enabled\n");
    }
#endif

    return true;
}

void OpenGLRenderAPI::destroyOpenGLContext()
{
    if (gl_context)
    {
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
    }
    window_handle = nullptr;
}

void OpenGLRenderAPI::setupOpenGLDefaults()
{
    // Enable depth testing
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearDepth(1.0);

    // Enable face culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Set default lighting (stored in member variables for shader uniforms)
    lighting_enabled = true;
    current_light_ambient = glm::vec3(0.2f, 0.2f, 0.2f);
    current_light_diffuse = glm::vec3(0.8f, 0.8f, 0.8f);
    current_light_position = glm::vec3(1.0f, 1.0f, 1.0f);
}

void OpenGLRenderAPI::beginFrame()
{
    // Reset model matrix to identity
    current_model_matrix = glm::mat4(1.0f);

    // Clear the matrix stack
    while (!model_matrix_stack.empty())
    {
        model_matrix_stack.pop();
    }
}

void OpenGLRenderAPI::endFrame()
{
    // Nothing specific needed for OpenGL
}

void OpenGLRenderAPI::present()
{
    if (window_handle)
    {
        SDL_GL_SwapWindow(window_handle);
    }
}

void OpenGLRenderAPI::clear(const vector3f& color)
{
    glClearColor(color.X, color.Y, color.Z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderAPI::setCamera(const camera& cam)
{
    vector3f pos = cam.getPosition();
    vector3f target = cam.getTarget();
    vector3f up = cam.getUpVector();

    // Set up view matrix using GLM
    view_matrix = glm::lookAt(
        glm::vec3(pos.X, pos.Y, pos.Z),
        glm::vec3(target.X, target.Y, target.Z),
        glm::vec3(up.X, up.Y, up.Z)
    );
}

void OpenGLRenderAPI::pushMatrix()
{
    model_matrix_stack.push(current_model_matrix);
}

void OpenGLRenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty())
    {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void OpenGLRenderAPI::translate(const vector3f& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, glm::vec3(pos.X, pos.Y, pos.Z));
}

void OpenGLRenderAPI::rotate(const matrix4f& rotation)
{
    glm::mat4 rot = convertToGLM(rotation);
    current_model_matrix = current_model_matrix * rot;
}

TextureHandle OpenGLRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;

    stbi_set_flip_vertically_on_load(invert_y);

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 0);
    if (!data)
    {
        fprintf(stderr, "Failed to load texture: %s\n", filename.c_str());
        return INVALID_TEXTURE;
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Determine format based on channels (OpenGL 4.6 Core compliant)
    GLenum format;
    GLenum internal_format;
    switch (channels)
    {
    case 1:
        internal_format = GL_R8;      // Modern replacement for GL_LUMINANCE
        format = GL_RED;
        break;
    case 3:
        internal_format = GL_RGB8;
        format = GL_RGB;
        break;
    case 4:
        internal_format = GL_RGBA8;
        format = GL_RGBA;
        break;
    default:
        fprintf(stderr, "Unsupported number of channels: %d\n", channels);
        stbi_image_free(data);
        glDeleteTextures(1, &texture);
        return INVALID_TEXTURE;
    }

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    // For single-channel textures, swizzle to replicate RED to RGB (grayscale behavior)
    if (channels == 1)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }

    // Generate mipmaps if requested (modern way)
    if (generate_mipmaps)
    {
        glGenerateMipmap(GL_TEXTURE_2D);  // Replaces gluBuild2DMipmaps
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Set texture wrapping
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);

    return (TextureHandle)texture;
}

void OpenGLRenderAPI::bindTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    }
    else
    {
        unbindTexture();
    }
}

void OpenGLRenderAPI::unbindTexture()
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        GLuint gl_texture = (GLuint)texture;
        glDeleteTextures(1, &gl_texture);
    }
}

void OpenGLRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded to GPU (lazy upload)
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU();
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        {
            return; // Upload failed
        }
    }

    // NOTE: Multi-material rendering is handled by renderer.hpp which calls
    // bindTexture() then renderMeshRange() for each material range.
    // This function only handles the simple single-texture rendering path.

    // Get appropriate shader for this render state
    Shader* shader = getShaderForRenderState(state);
    if (!shader || !shader->isValid())
    {
        return; // No valid shader
    }

    // Bind shader
    shader->use();

    // Set matrix uniforms
    shader->setUniform("uModel", current_model_matrix);
    shader->setUniform("uView", view_matrix);
    shader->setUniform("uProjection", projection_matrix);

    // Set lighting uniforms
    shader->setUniform("uLightPos", current_light_position);
    shader->setUniform("uLightAmbient", current_light_ambient);
    shader->setUniform("uLightDiffuse", current_light_diffuse);

    // Set color uniform
    shader->setUniform("uColor", glm::vec3(state.color.X, state.color.Y, state.color.Z));

    // Bind texture if available
    if (m.texture_set && m.texture != INVALID_TEXTURE)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)m.texture);
        shader->setUniform("uTexture", 0);
        shader->setUniform("uUseTexture", true);
    }
    else
    {
        shader->setUniform("uUseTexture", false);
    }

    // Apply render state (culling, blending, depth)
    applyRenderState(state);

    // Bind VAO and draw
    m.gpu_mesh->bind();
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m.gpu_mesh->getVertexCount()));
    m.gpu_mesh->unbind();

    // Unbind texture
    if (m.texture_set && m.texture != INVALID_TEXTURE)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Reset some states after rendering to prevent bleeding
    if (state.blend_mode != BlendMode::None)
    {
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}

void OpenGLRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0) return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len)
    {
        // Clamp to valid range
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded to GPU (lazy upload)
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU();
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        {
            return; // Upload failed
        }
    }

    // Get appropriate shader for this render state
    Shader* shader = getShaderForRenderState(state);
    if (!shader || !shader->isValid())
    {
        return; // No valid shader
    }

    // Bind shader
    shader->use();

    // Set matrix uniforms
    shader->setUniform("uModel", current_model_matrix);
    shader->setUniform("uView", view_matrix);
    shader->setUniform("uProjection", projection_matrix);

    // Set lighting uniforms
    shader->setUniform("uLightPos", current_light_position);
    shader->setUniform("uLightAmbient", current_light_ambient);
    shader->setUniform("uLightDiffuse", current_light_diffuse);

    // Set color uniform
    shader->setUniform("uColor", glm::vec3(state.color.X, state.color.Y, state.color.Z));

    // Texture binding logic:
    // - For single-texture meshes: bind m.texture
    // - For multi-material meshes: texture already bound by renderer.hpp, just enable it
    if (!m.uses_material_ranges && m.texture_set && m.texture != INVALID_TEXTURE)
    {
        // Single-texture mode: bind the mesh's texture
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)m.texture);
        shader->setUniform("uTexture", 0);
        shader->setUniform("uUseTexture", true);
    }
    else if (m.uses_material_ranges)
    {
        // Multi-material mode: texture already bound by renderer.hpp
        // Just tell the shader to use whatever is bound at texture unit 0
        shader->setUniform("uTexture", 0);
        shader->setUniform("uUseTexture", true);
    }
    else
    {
        // No texture available
        shader->setUniform("uUseTexture", false);
    }

    // Apply render state (culling, blending, depth)
    applyRenderState(state);

    // Bind VAO and draw range
    m.gpu_mesh->bind();
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(start_vertex), static_cast<GLsizei>(vertex_count));
    m.gpu_mesh->unbind();

    // Unbind texture only for single-texture meshes (multi-material handled by renderer.hpp)
    if (!m.uses_material_ranges && m.texture_set && m.texture != INVALID_TEXTURE)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Reset some states after rendering to prevent bleeding
    if (state.blend_mode != BlendMode::None)
    {
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}

void OpenGLRenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    applyRenderState(state);
}

void OpenGLRenderAPI::applyRenderState(const RenderState& state)
{
    // Culling
    if (state.cull_mode == CullMode::None)
    {
        glDisable(GL_CULL_FACE);
    }
    else
    {
        glEnable(GL_CULL_FACE);
        glCullFace(getGLCullMode(state.cull_mode));
    }

    // Blending
    setupBlending(state.blend_mode);

    // Depth testing
    setupDepthTesting(state.depth_test, state.depth_write);

    // Lighting
    enableLighting(state.lighting);
}

GLenum OpenGLRenderAPI::getGLCullMode(CullMode mode)
{
    switch (mode)
    {
    case CullMode::Back: return GL_BACK;
    case CullMode::Front: return GL_FRONT;
    default: return GL_BACK;
    }
}

void OpenGLRenderAPI::setupBlending(BlendMode mode)
{
    switch (mode)
    {
    case BlendMode::None:
        glDisable(GL_BLEND);
        break;
    case BlendMode::Alpha:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BlendMode::Additive:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    }
}

void OpenGLRenderAPI::setupDepthTesting(DepthTest test, bool write)
{
    if (test == DepthTest::None)
    {
        glDisable(GL_DEPTH_TEST);
    }
    else
    {
        glEnable(GL_DEPTH_TEST);
        switch (test)
        {
        case DepthTest::Less:
            glDepthFunc(GL_LESS);
            break;
        case DepthTest::LessEqual:
            glDepthFunc(GL_LEQUAL);
            break;
        default:
            glDepthFunc(GL_LEQUAL);
            break;
        }
    }

    glDepthMask(write ? GL_TRUE : GL_FALSE);
}

void OpenGLRenderAPI::enableLighting(bool enable)
{
    // Store lighting state for shader usage
    lighting_enabled = enable;
}

void OpenGLRenderAPI::setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& position)
{
    // Store lighting parameters for shader uniforms
    current_light_ambient = glm::vec3(ambient.X, ambient.Y, ambient.Z);
    current_light_diffuse = glm::vec3(diffuse.X, diffuse.Y, diffuse.Z);
    current_light_position = glm::vec3(position.X, position.Y, position.Z);
}

// Factory implementation
IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::OpenGL:
        return new OpenGLRenderAPI();
    default:
        return nullptr;
    }
}

void OpenGLRenderAPI::multiplyMatrix(const matrix4f& matrix)
{
    glm::mat4 mat = convertToGLM(matrix);
    current_model_matrix = current_model_matrix * mat;
}

// OpenGL debug callback for error reporting
void GLAPIENTRY OpenGLRenderAPI::openglDebugCallback(GLenum source, GLenum type, GLuint id,
                                                      GLenum severity, GLsizei length,
                                                      const GLchar* message, const void* userParam)
{
    // Ignore non-significant error/warning codes
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204) return;

    printf("OpenGL Debug Message:\n");
    printf("  Source: ");
    switch (source)
    {
    case GL_DEBUG_SOURCE_API:             printf("API"); break;
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   printf("Window System"); break;
    case GL_DEBUG_SOURCE_SHADER_COMPILER: printf("Shader Compiler"); break;
    case GL_DEBUG_SOURCE_THIRD_PARTY:     printf("Third Party"); break;
    case GL_DEBUG_SOURCE_APPLICATION:     printf("Application"); break;
    case GL_DEBUG_SOURCE_OTHER:           printf("Other"); break;
    }
    printf("\n");

    printf("  Type: ");
    switch (type)
    {
    case GL_DEBUG_TYPE_ERROR:               printf("Error"); break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: printf("Deprecated Behaviour"); break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  printf("Undefined Behaviour"); break;
    case GL_DEBUG_TYPE_PORTABILITY:         printf("Portability"); break;
    case GL_DEBUG_TYPE_PERFORMANCE:         printf("Performance"); break;
    case GL_DEBUG_TYPE_MARKER:              printf("Marker"); break;
    case GL_DEBUG_TYPE_PUSH_GROUP:          printf("Push Group"); break;
    case GL_DEBUG_TYPE_POP_GROUP:           printf("Pop Group"); break;
    case GL_DEBUG_TYPE_OTHER:               printf("Other"); break;
    }
    printf("\n");

    printf("  Severity: ");
    switch (severity)
    {
    case GL_DEBUG_SEVERITY_HIGH:         printf("High"); break;
    case GL_DEBUG_SEVERITY_MEDIUM:       printf("Medium"); break;
    case GL_DEBUG_SEVERITY_LOW:          printf("Low"); break;
    case GL_DEBUG_SEVERITY_NOTIFICATION: printf("Notification"); break;
    }
    printf("\n");

    printf("  Message: %s\n\n", message);
}

// Helper to convert irrlicht matrix4f to glm::mat4
glm::mat4 OpenGLRenderAPI::convertToGLM(const matrix4f& m) const
{
    const float* ptr = m.pointer();
    return glm::make_mat4(ptr);
}

// Get appropriate shader for render state
Shader* OpenGLRenderAPI::getShaderForRenderState(const RenderState& state)
{
    if (!shader_manager) return nullptr;

    if (state.lighting && lighting_enabled)
    {
        return shader_manager->getShader("basic");
    }
    else
    {
        return shader_manager->getShader("unlit");
    }
}