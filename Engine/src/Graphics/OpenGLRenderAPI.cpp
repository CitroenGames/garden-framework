#include "OpenGLRenderAPI.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/ShaderManager.hpp"
#include "Graphics/OpenGLMesh.hpp"
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

OpenGLRenderAPI::OpenGLRenderAPI()
    : window_handle(nullptr), gl_context(nullptr), viewport_width(0), viewport_height(0), field_of_view(75.0f),
      projection_matrix(1.0f), view_matrix(1.0f), current_model_matrix(1.0f),
      current_light_direction(0.0f, -1.0f, 0.0f), current_light_ambient(0.2f, 0.2f, 0.2f),
      current_light_diffuse(0.8f, 0.8f, 0.8f), lighting_enabled(true), shader_manager(nullptr),
      post_processing(nullptr), skybox(nullptr),
      shadowMapFBO(0), shadowMapTexture(0), lightSpaceMatrix(1.0f), in_shadow_pass(false),
      current_shader_id(0), current_bound_texture_0(0), global_uniforms_dirty(true)
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
    
    // Load FXAA shader
    shader_manager->loadShader("fxaa", "assets/shaders/fxaa.vert", "assets/shaders/fxaa.frag");
    
    // Load Sky shader
    shader_manager->loadShader("sky", "assets/shaders/sky.vert", "assets/shaders/sky.frag");

    // Load Shadow shader
    shader_manager->loadShader("shadow", "assets/shaders/shadow.vert", "assets/shaders/shadow.frag");

    // Configure Shadow Map FBO
    glGenFramebuffers(1, &shadowMapFBO);
    
    glGenTextures(1, &shadowMapTexture);
    glBindTexture(GL_TEXTURE_2D, shadowMapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 
                 SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); 
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMapTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Initialize PostProcessing
    post_processing = new PostProcessing();
    if (!post_processing->initialize(width, height, shader_manager->getShader("fxaa")))
    {
        printf("Failed to initialize PostProcessing\n");
    }

    // Initialize Skybox
    skybox = new Skybox();
    if (!skybox->initialize("textures/t_sky.png", this, shader_manager->getShader("sky")))
    {
        printf("Failed to initialize Skybox\n");
    }

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

    if (post_processing)
    {
        delete post_processing;
        post_processing = nullptr;
    }

    if (skybox)
    {
        delete skybox;
        skybox = nullptr;
    }

    if (shadowMapFBO) 
    {
        glDeleteFramebuffers(1, &shadowMapFBO);
        shadowMapFBO = 0;
    }
    
    if (shadowMapTexture) 
    {
        glDeleteTextures(1, &shadowMapTexture);
        shadowMapTexture = 0;
    }

    // Note: No need to disable GL capabilities during shutdown
    // The context is about to be destroyed anyway

    destroyOpenGLContext();
}

void OpenGLRenderAPI::resize(int width, int height)
{
    viewport_width = width;
    viewport_height = height;

    if (post_processing)
    {
        post_processing->resize(width, height);
    }

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
    
    global_uniforms_dirty = true;
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
    current_light_direction = glm::vec3(0.0f, -1.0f, 0.0f);
}

void OpenGLRenderAPI::beginFrame()
{
    if (post_processing)
    {
        post_processing->beginRender();
        // Ensure GL state matches our assumption
        glDepthFunc(GL_LEQUAL); 
        current_gpu_state.depth_test = DepthTest::LessEqual;
    }

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
    if (post_processing)
    {
        post_processing->endRender();
        post_processing->renderFXAA();

        // PostProcessing modifies GL state
        current_shader_id = 0;
        current_bound_texture_0 = 0;
        
        // renderFXAA disables depth test
        current_gpu_state.depth_test = DepthTest::None;
    }
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
    
    global_uniforms_dirty = true;
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
        GLuint texID = (GLuint)texture;
        if (current_bound_texture_0 != texID)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texID);
            current_bound_texture_0 = texID;
        }
    }
    else
    {
        unbindTexture();
    }
}

void OpenGLRenderAPI::unbindTexture()
{
    if (current_bound_texture_0 != 0)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        current_bound_texture_0 = 0;
    }
}

void OpenGLRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        GLuint gl_texture = (GLuint)texture;
        glDeleteTextures(1, &gl_texture);
    }
}

void OpenGLRenderAPI::beginShadowPass(const vector3f& lightDir)
{
    in_shadow_pass = true;
    
    // Set up light space matrix
    // Orthographic projection for directional light
    float near_plane = 1.0f, far_plane = 200.0f;
    float ortho_size = 50.0f; // Adjust based on scene scale
    glm::mat4 lightProjection = glm::ortho(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane);
    
    // View matrix looking from light direction
    // For directional light, position can be arbitrary but should cover the scene
    // We position it 'far away' along the reverse light direction
    glm::vec3 direction = glm::normalize(glm::vec3(lightDir.X, lightDir.Y, lightDir.Z));
    // Convention: light direction points FROM light, so negate to look AT origin
    // Wait, the level data stores direction vector (e.g., 0, -1, 0 for down). 
    // So light comes FROM -direction * distance.
    glm::vec3 lightPos = -direction * 100.0f; 
    
    glm::mat4 lightView = glm::lookAt(lightPos, 
                                      glm::vec3(0.0f, 0.0f, 0.0f), 
                                      glm::vec3(0.0f, 1.0f, 0.0f));
                                      
    lightSpaceMatrix = lightProjection * lightView;
    
    // Bind FBO and set viewport
    glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    
    // Enable front-face culling to fix peter panning
    glCullFace(GL_FRONT);
    
    global_uniforms_dirty = true;
}

void OpenGLRenderAPI::endShadowPass()
{
    in_shadow_pass = false;
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewport_width, viewport_height);
    
    // Restore back-face culling
    glCullFace(GL_BACK);
}

void OpenGLRenderAPI::bindShadowMap(int textureUnit)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, shadowMapTexture);
}

matrix4f OpenGLRenderAPI::getLightSpaceMatrix()
{
    matrix4f m;
    const float* src = glm::value_ptr(lightSpaceMatrix);
    float* dst = m.pointer();
    for(int i=0; i<16; ++i) dst[i] = src[i];
    return m;
}

void OpenGLRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded to GPU (lazy upload)
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        {
            return; // Upload failed
        }
    }

    // NOTE: Multi-material rendering is handled by renderer.hpp which calls
    // bindTexture() then renderMeshRange() for each material range.
    // This function only handles the simple single-texture rendering path.

    // Get appropriate shader for this render state
    Shader* shader = nullptr;
    
    if (in_shadow_pass)
    {
        shader = shader_manager->getShader("shadow");
    }
    else
    {
        shader = getShaderForRenderState(state);
    }
    
    if (!shader || !shader->isValid())
    {
        return; // No valid shader
    }

    // Bind shader
    bool shader_changed = false;
    if (current_shader_id != shader->getProgramID())
    {
        shader->use();
        current_shader_id = shader->getProgramID();
        shader_changed = true;
    }

    if (in_shadow_pass)
    {
        if (shader_changed || global_uniforms_dirty)
        {
             shader->setUniform("uLightSpaceMatrix", lightSpaceMatrix);
             // We can safely clear dirty flag here if we assume this is the only global needed for shadow pass
             // BUT, if we clear it, then subsequent Normal Pass won't update its globals (View/Proj).
             // Ideally we should have different dirty flags or simply NOT clear it here if it might affect other shaders.
             // However, shader_changed logic in Normal Pass will catch the switch back to "basic" shader.
             // So clearing it here is safe provided we switch shaders.
             // The only risk is if we use SAME shader for shadow and normal pass (unlikely).
             // Or if we have multiple shadow shaders?
             // Let's NOT clear it here to be safe, because this update is partial (only LightSpace).
        }
        shader->setUniform("uModel", current_model_matrix);
    }
    else
    {
        // Set global uniforms if shader changed OR globals are dirty
        if (shader_changed || global_uniforms_dirty)
        {
            shader->setUniform("uView", view_matrix);
            shader->setUniform("uProjection", projection_matrix);
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrix);

            // Set lighting uniforms
            shader->setUniform("uLightDir", current_light_direction);
            shader->setUniform("uLightAmbient", current_light_ambient);
            shader->setUniform("uLightDiffuse", current_light_diffuse);
            
            // Bind Shadow Map (Unit 1) - effectively global
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, shadowMapTexture);
            shader->setUniform("uShadowMap", 1);
            
            // Now we can clear the dirty flag because we updated the "main" globals
            // CAUTION: This clears it for THIS shader.
            // If we have multiple shaders in the scene (e.g. Basic, Unlit, Terrain),
            // and we render Mesh A (Basic), Mesh B (Terrain), Mesh C (Basic).
            // 1. Basic. Upload. Dirty=false.
            // 2. Terrain. Changed=true. Upload. Dirty=false.
            // 3. Basic. Changed=true. Upload. Dirty=false.
            // This works!
            // Case: Mesh A (Basic). Dirty=true. Upload. Dirty=false.
            //       Mesh B (Basic). Changed=false. Dirty=false. Skip.
            // Works!
            global_uniforms_dirty = false;
        }

        // Set object specific uniforms
        shader->setUniform("uModel", current_model_matrix);
        shader->setUniform("uColor", glm::vec3(state.color.X, state.color.Y, state.color.Z));

        // Bind texture if available (Unit 0)
        if (m.texture_set && m.texture != INVALID_TEXTURE)
        {
            // Use our optimized bindTexture
            bindTexture(m.texture);
            shader->setUniform("uTexture", 0);
            shader->setUniform("uUseTexture", true);
        }
        else
        {
            shader->setUniform("uUseTexture", false);
        }
    }

    // Apply render state (culling, blending, depth)
    applyRenderState(state);

    // Bind VAO and draw
    OpenGLMesh* glMesh = dynamic_cast<OpenGLMesh*>(m.gpu_mesh);
    if (glMesh)
    {
        glMesh->bind();
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(glMesh->getVertexCount()));
        glMesh->unbind();
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
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        {
            return; // Upload failed
        }
    }

    // Get appropriate shader for this render state
    Shader* shader = nullptr;
    
    if (in_shadow_pass)
    {
        shader = shader_manager->getShader("shadow");
    }
    else
    {
        shader = getShaderForRenderState(state);
    }
    
    if (!shader || !shader->isValid())
    {
        return; // No valid shader
    }

    // Bind shader
    bool shader_changed = false;
    if (current_shader_id != shader->getProgramID())
    {
        shader->use();
        current_shader_id = shader->getProgramID();
        shader_changed = true;
    }

    if (in_shadow_pass)
    {
        if (shader_changed || global_uniforms_dirty)
        {
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrix);
        }
        shader->setUniform("uModel", current_model_matrix);
    }
    else
    {
        if (shader_changed || global_uniforms_dirty)
        {
            // Set matrix uniforms
            shader->setUniform("uView", view_matrix);
            shader->setUniform("uProjection", projection_matrix);
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrix);

            // Set lighting uniforms
            shader->setUniform("uLightDir", current_light_direction);
            shader->setUniform("uLightAmbient", current_light_ambient);
            shader->setUniform("uLightDiffuse", current_light_diffuse);
            
            // Bind Shadow Map (Unit 1)
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, shadowMapTexture);
            shader->setUniform("uShadowMap", 1);
            
            global_uniforms_dirty = false;
        }

        // Set object specific uniforms
        shader->setUniform("uModel", current_model_matrix);
        shader->setUniform("uColor", glm::vec3(state.color.X, state.color.Y, state.color.Z));

        // Texture binding logic:
        // - For single-texture meshes: bind m.texture
        // - For multi-material meshes: texture already bound by renderer.hpp, just enable it
        if (!m.uses_material_ranges && m.texture_set && m.texture != INVALID_TEXTURE)
        {
            // Single-texture mode: bind the mesh's texture
            bindTexture(m.texture);
            shader->setUniform("uTexture", 0);
            shader->setUniform("uUseTexture", true);
        }
        else if (m.uses_material_ranges)
        {
            // Multi-material mode: texture already bound by renderer.hpp
            // Note: renderer.hpp likely calls bindTexture directly, which updates our tracker.
            // Just tell the shader to use whatever is bound at texture unit 0
            shader->setUniform("uTexture", 0);
            shader->setUniform("uUseTexture", true);
        }
        else
        {
            // No texture available
            shader->setUniform("uUseTexture", false);
        }
    }

    // Apply render state (culling, blending, depth)
    applyRenderState(state);

    // Bind VAO and draw range
    OpenGLMesh* glMesh = dynamic_cast<OpenGLMesh*>(m.gpu_mesh);
    if (glMesh)
    {
        glMesh->bind();
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(start_vertex), static_cast<GLsizei>(vertex_count));
        glMesh->unbind();
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
    if (state.cull_mode != current_gpu_state.cull_mode)
    {
        if (state.cull_mode == CullMode::None)
        {
            glDisable(GL_CULL_FACE);
        }
        else
        {
            if (current_gpu_state.cull_mode == CullMode::None)
            {
                glEnable(GL_CULL_FACE);
            }
            glCullFace(getGLCullMode(state.cull_mode));
        }
        current_gpu_state.cull_mode = state.cull_mode;
    }

    // Blending
    if (state.blend_mode != current_gpu_state.blend_mode)
    {
        setupBlending(state.blend_mode);
        current_gpu_state.blend_mode = state.blend_mode;
    }

    // Depth testing
    if (state.depth_test != current_gpu_state.depth_test || state.depth_write != current_gpu_state.depth_write)
    {
        setupDepthTesting(state.depth_test, state.depth_write);
        current_gpu_state.depth_test = state.depth_test;
        current_gpu_state.depth_write = state.depth_write;
    }

    // Lighting
    if (state.lighting != current_gpu_state.lighting)
    {
        enableLighting(state.lighting);
        current_gpu_state.lighting = state.lighting;
    }
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

void OpenGLRenderAPI::setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& direction)
{
    // Store lighting parameters for shader uniforms
    current_light_ambient = glm::vec3(ambient.X, ambient.Y, ambient.Z);
    current_light_diffuse = glm::vec3(diffuse.X, diffuse.Y, diffuse.Z);
    current_light_direction = glm::vec3(direction.X, direction.Y, direction.Z);
    // Ensure direction is normalized
    current_light_direction = glm::normalize(current_light_direction);
    
    global_uniforms_dirty = true;
}

void OpenGLRenderAPI::multiplyMatrix(const matrix4f& matrix)
{
    glm::mat4 mat = convertToGLM(matrix);
    current_model_matrix = current_model_matrix * mat;
}

void OpenGLRenderAPI::renderSkybox()
{
    if (skybox)
    {
        matrix4f view;
        view.setM(glm::value_ptr(view_matrix));
        
        matrix4f proj;
        proj.setM(glm::value_ptr(projection_matrix));
        
        skybox->render(view, proj);

        // Skybox modifies GL state, so we need to sync our trackers
        current_shader_id = 0;
        current_bound_texture_0 = 0;
        
        // Skybox::render sets DepthFunc to GL_LESS at the end
        current_gpu_state.depth_test = DepthTest::Less;
    }
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

IGPUMesh* OpenGLRenderAPI::createMesh()
{
    return new OpenGLMesh();
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