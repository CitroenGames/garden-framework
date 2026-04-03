#include "OpenGLRenderAPI.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/ShaderManager.hpp"
#include "Graphics/OpenGLMesh.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include "Utils/Log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

OpenGLRenderAPI::OpenGLRenderAPI()
    : window_handle(nullptr), gl_context(nullptr), viewport_width(0), viewport_height(0), field_of_view(75.0f),
      projection_matrix(1.0f), view_matrix(1.0f), current_model_matrix(1.0f),
      current_light_direction(0.0f, -1.0f, 0.0f), current_light_ambient(0.2f, 0.2f, 0.2f),
      current_light_diffuse(0.8f, 0.8f, 0.8f), lighting_enabled(true), shader_manager(nullptr),
      post_processing(nullptr), skybox(nullptr),
      shadowMapFBO(0), shadowMapTextureArray(0), lightSpaceMatrix(1.0f), in_shadow_pass(false),
      currentCascade(0), cascadeSplitLambda(0.92f), debugCascades(false),
      current_shader_id(0), current_bound_texture_0(0), global_uniforms_dirty(true)
{
    // Initialize cascade arrays
    for (int i = 0; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = glm::mat4(1.0f);
    }
    for (int i = 0; i <= NUM_CASCADES; i++) {
        cascadeSplitDistances[i] = 0.0f;
    }
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
    shader_manager = std::make_unique<ShaderManager>();
    shader_manager->createDefaultShaders();
    
    // Load FXAA shader
    shader_manager->loadShader("fxaa", "assets/shaders/fxaa.vert", "assets/shaders/fxaa.frag");
    
    // Load Sky shader
    shader_manager->loadShader("sky", "assets/shaders/sky.vert", "assets/shaders/sky.frag");

    // Load Shadow shader
    shader_manager->loadShader("shadow", "assets/shaders/shadow.vert", "assets/shaders/shadow.frag");

    // Load Depth prepass shader
    shader_manager->loadShader("depth", "assets/shaders/depth.vert", "assets/shaders/depth.frag");

    // Configure Shadow Map FBO with Cascaded Shadow Maps (texture array)
    glGenFramebuffers(1, &shadowMapFBO);

    // Create 2D texture array for CSM cascades
    glGenTextures(1, &shadowMapTextureArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F,
                 currentShadowSize, currentShadowSize, NUM_CASCADES,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
    // Attach first layer initially (will switch per cascade)
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowMapTextureArray, 0, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    // Verify FBO is complete
    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ENGINE_ERROR("CSM Shadow FBO incomplete! Status: {}", fboStatus);
    } else {
        LOG_ENGINE_INFO("CSM Shadow FBO created successfully ({}x{} x {} cascades)",
            currentShadowSize, currentShadowSize, NUM_CASCADES);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Initialize cascade split distances
    calculateCascadeSplits(0.1f, 1000.0f);
    LOG_ENGINE_INFO("CSM Cascade splits: {:.2f}, {:.2f}, {:.2f}, {:.2f}, {:.2f}",
        cascadeSplitDistances[0], cascadeSplitDistances[1],
        cascadeSplitDistances[2], cascadeSplitDistances[3],
        cascadeSplitDistances[4]);

    // Debug cascade visualization (toggle via console command later)
    debugCascades = false;

    // Initialize PostProcessing
    post_processing = std::make_unique<PostProcessing>();
    if (!post_processing->initialize(width, height, shader_manager->getShader("fxaa")))
    {
        printf("Failed to initialize PostProcessing\n");
    }

    // Initialize Skybox
    skybox = std::make_unique<Skybox>();
    if (!skybox->initialize(shader_manager->getShader("sky")))
    {
        printf("Failed to initialize Skybox\n");
    }

    // Create Uniform Buffer Objects
    glGenBuffers(1, &camera_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(CameraUBOData), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, camera_ubo);

    glGenBuffers(1, &lighting_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, lighting_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(LightingUBOData), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, lighting_ubo);

    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    LOG_ENGINE_INFO("UBOs created: CameraData={}B, LightingData={}B", sizeof(CameraUBOData), sizeof(LightingUBOData));

    setupOpenGLDefaults();
    resize(width, height);

    printf("OpenGL Render API initialized (%dx%d, FOV: %.1f)\n", width, height, fov);
    return true;
}

void OpenGLRenderAPI::shutdown()
{
    // Clean up debug line VBO persistent mapping
    if (debug_vbo_mapped_ptr && debug_vbo)
    {
        glBindBuffer(GL_ARRAY_BUFFER, debug_vbo);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        debug_vbo_mapped_ptr = nullptr;
    }
    if (debug_vbo) { glDeleteBuffers(1, &debug_vbo); debug_vbo = 0; }
    if (debug_vao) { glDeleteVertexArrays(1, &debug_vao); debug_vao = 0; }
    debug_vbo_capacity = 0;

    // Clean up shader manager and rendering subsystems
    shader_manager.reset();
    post_processing.reset();
    skybox.reset();

    destroyViewportFBO();

    if (camera_ubo)
    {
        glDeleteBuffers(1, &camera_ubo);
        camera_ubo = 0;
    }
    if (lighting_ubo)
    {
        glDeleteBuffers(1, &lighting_ubo);
        lighting_ubo = 0;
    }

    if (shadowMapFBO)
    {
        glDeleteFramebuffers(1, &shadowMapFBO);
        shadowMapFBO = 0;
    }

    if (shadowMapTextureArray)
    {
        glDeleteTextures(1, &shadowMapTextureArray);
        shadowMapTextureArray = 0;
    }

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
        1000.0f
    );

    // Set up viewport
    glViewport(0, 0, width, height);

    camera_ubo_dirty = true;
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

void OpenGLRenderAPI::uploadCameraUBO()
{
    CameraUBOData data;
    data.view = view_matrix;
    data.projection = projection_matrix;
    data.cameraPos = glm::vec4(0.0f); // camera pos not currently used in shaders

    glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CameraUBOData), &data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    camera_ubo_dirty = false;
}

void OpenGLRenderAPI::uploadLightingUBO()
{
    LightingUBOData data;
    for (int i = 0; i < NUM_CASCADES; i++)
        data.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    for (int i = 0; i <= NUM_CASCADES; i++)
        data.cascadeSplits[i] = glm::vec4(cascadeSplitDistances[i], 0.0f, 0.0f, 0.0f);
    data.cascadeParams = glm::ivec4(NUM_CASCADES, debugCascades ? 1 : 0, 0, 0);
    data.lightDir = glm::vec4(current_light_direction, 0.0f);
    data.lightAmbient = glm::vec4(current_light_ambient, 0.0f);
    data.lightDiffuse = glm::vec4(current_light_diffuse, 0.0f);

    glBindBuffer(GL_UNIFORM_BUFFER, lighting_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(LightingUBOData), &data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    lighting_ubo_dirty = false;
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

    // Upload UBOs if dirty
    if (camera_ubo_dirty)
        uploadCameraUBO();
    if (lighting_ubo_dirty)
        uploadLightingUBO();

    // Bind shadow map array to texture unit 1 once per frame
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);

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

        if (fxaaEnabled)
        {
            post_processing->renderFXAA();
        }
        else
        {
            post_processing->renderPassthrough();
        }

        // PostProcessing modifies GL state
        current_shader_id = 0;
        current_bound_texture_0 = 0;

        // renderFXAA disables depth test
        current_gpu_state.depth_test = DepthTest::None;
    }

    // Render ImGui AFTER FXAA so UI text stays crisp
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }
}

void OpenGLRenderAPI::present()
{
    if (window_handle)
    {
        SDL_GL_SwapWindow(window_handle);
    }
}

// --- Viewport render-to-texture (for editor) ---

void OpenGLRenderAPI::createViewportFBO(int w, int h)
{
    if (viewport_fbo) destroyViewportFBO();

    viewport_width_rt = w;
    viewport_height_rt = h;

    glGenFramebuffers(1, &viewport_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, viewport_fbo);

    glGenTextures(1, &viewport_texture);
    glBindTexture(GL_TEXTURE_2D, viewport_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewport_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        printf("ERROR: Viewport framebuffer not complete!\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderAPI::destroyViewportFBO()
{
    if (viewport_fbo) { glDeleteFramebuffers(1, &viewport_fbo); viewport_fbo = 0; }
    if (viewport_texture) { glDeleteTextures(1, &viewport_texture); viewport_texture = 0; }
    viewport_width_rt = 0;
    viewport_height_rt = 0;
}

void OpenGLRenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == viewport_width_rt && height == viewport_height_rt) return;

    createViewportFBO(width, height);

    // Also resize the post-processing FBO to match the scene render size
    if (post_processing)
        post_processing->resize(width, height);
}

void OpenGLRenderAPI::endSceneRender()
{
    if (!post_processing) return;

    // End scene rendering to post-processing FBO
    post_processing->endRender();

    // Now apply FXAA (or passthrough) to the viewport texture
    if (viewport_fbo)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, viewport_fbo);
        glViewport(0, 0, viewport_width_rt, viewport_height_rt);
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, viewport_width, viewport_height);
    }

    if (fxaaEnabled)
    {
        Shader* fxaa_shader = post_processing->getFXAAShader();
        if (fxaa_shader)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            fxaa_shader->use();
            fxaa_shader->setUniform("uInverseScreenSize",
                glm::vec2(1.0f / viewport_width_rt, 1.0f / viewport_height_rt));

            glBindVertexArray(post_processing->getQuadVAO());
            glDisable(GL_DEPTH_TEST);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, post_processing->getColorTexture());
            fxaa_shader->setUniform("screenTexture", 0);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
        }
    }
    else
    {
        // Blit from post-processing FBO to viewport FBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, post_processing->getFramebuffer());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, viewport_fbo ? viewport_fbo : 0);
        glBlitFramebuffer(0, 0, post_processing->getWidth(), post_processing->getHeight(),
                          0, 0, viewport_width_rt, viewport_height_rt,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    // Unbind and restore
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewport_width, viewport_height);

    // Reset state tracking since we modified GL state
    current_shader_id = 0;
    current_bound_texture_0 = 0;
    current_gpu_state.depth_test = DepthTest::None;
}

uint64_t OpenGLRenderAPI::getViewportTextureID()
{
    return static_cast<ImU64>(viewport_texture);
}

void OpenGLRenderAPI::renderUI()
{
    // Bind screen backbuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewport_width, viewport_height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    // Render ImGui draw data to screen
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }
}

void OpenGLRenderAPI::clear(const glm::vec3& color)
{
    glClearColor(color.x, color.y, color.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();

    // Set up view matrix using GLM
    view_matrix = glm::lookAt(pos, target, up);

    camera_ubo_dirty = true;
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

void OpenGLRenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void OpenGLRenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
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

TextureHandle OpenGLRenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                     bool flip_vertically, bool generate_mipmaps)
{
    if (!pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4)
    {
        fprintf(stderr, "Invalid texture data for loadTextureFromMemory\n");
        return INVALID_TEXTURE;
    }

    const uint8_t* data = pixels;
    std::vector<uint8_t> flipped_data;

    if (flip_vertically)
    {
        size_t row_size = static_cast<size_t>(width) * channels;
        flipped_data.resize(static_cast<size_t>(width) * height * channels);
        for (int y = 0; y < height; ++y)
        {
            std::memcpy(flipped_data.data() + y * row_size,
                       pixels + (height - 1 - y) * row_size,
                       row_size);
        }
        data = flipped_data.data();
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    GLenum format;
    GLenum internal_format;
    switch (channels)
    {
    case 1:
        internal_format = GL_R8;
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
        glDeleteTextures(1, &texture);
        return INVALID_TEXTURE;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    if (channels == 1)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }

    if (generate_mipmaps)
    {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

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

// CSM Helper: Calculate cascade split distances using practical split scheme
void OpenGLRenderAPI::calculateCascadeSplits(float nearPlane, float farPlane)
{
    cascadeSplitDistances[0] = nearPlane;
    for (int i = 1; i <= NUM_CASCADES; i++) {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float linear = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log + (1.0f - cascadeSplitLambda) * linear;
    }
}

// CSM Helper: Get frustum corners in world space
std::array<glm::vec3, 8> OpenGLRenderAPI::getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
{
    const glm::mat4 inv = glm::inverse(proj * view);
    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                glm::vec4 pt = inv * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    2.0f * z - 1.0f,
                    1.0f);
                corners[idx++] = glm::vec3(pt) / pt.w;
            }
        }
    }
    return corners;
}

// CSM Helper: Calculate light space matrix for a specific cascade
glm::mat4 OpenGLRenderAPI::getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
    const glm::mat4& viewMatrix, float fov, float aspect)
{
    // Get cascade near/far
    float cascadeNear = cascadeSplitDistances[cascadeIndex];
    float cascadeFar = cascadeSplitDistances[cascadeIndex + 1];

    // Create projection for this cascade's frustum slice
    glm::mat4 cascadeProj = glm::perspective(glm::radians(fov), aspect, cascadeNear, cascadeFar);

    // Get frustum corners in world space
    auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMatrix);

    // Calculate frustum center
    glm::vec3 center(0.0f);
    for (const auto& c : corners) {
        center += c;
    }
    center /= 8.0f;

    // Light view matrix
    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f) {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::mat4 lightView = glm::lookAt(
        center - direction * 100.0f,  // Position light away from center
        center,
        up);

    // Find bounding box in light space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (const auto& c : corners) {
        glm::vec4 lsCorner = lightView * glm::vec4(c, 1.0f);
        minX = std::min(minX, lsCorner.x);
        maxX = std::max(maxX, lsCorner.x);
        minY = std::min(minY, lsCorner.y);
        maxY = std::max(maxY, lsCorner.y);
        minZ = std::min(minZ, lsCorner.z);
        maxZ = std::max(maxZ, lsCorner.z);
    }

    // Add padding to prevent edge artifacts
    // Extend Z-bounds significantly to capture casters in front of the frustum
    float padding = 10.0f;
    minZ -= padding;
    maxZ += 500.0f; // Extend towards light source

    // Orthographic projection tightly fitted to frustum
    glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);

    return lightProj * lightView;
}

// Legacy beginShadowPass - uses first cascade only for backwards compatibility
void OpenGLRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    // Skip if shadows are disabled
    if (shadowQuality == 0 || shadowMapFBO == 0)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // Set up light space matrix for cascade 0 only (legacy mode)
    float near_plane = 1.0f, far_plane = 1000.0f;
    float ortho_size = 50.0f;
    glm::mat4 lightProjection = glm::ortho(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane);

    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 lightPos = -direction * 100.0f;

    glm::mat4 lightView = glm::lookAt(lightPos,
                                      glm::vec3(0.0f, 0.0f, 0.0f),
                                      glm::vec3(0.0f, 1.0f, 0.0f));

    lightSpaceMatrix = lightProjection * lightView;
    lightSpaceMatrices[0] = lightSpaceMatrix;

    // Bind FBO and set viewport
    glViewport(0, 0, currentShadowSize, currentShadowSize);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowMapTextureArray, 0, 0);
    glClear(GL_DEPTH_BUFFER_BIT);

    currentCascade = 0;
    global_uniforms_dirty = true;
}

// CSM beginShadowPass - calculates all cascade matrices
void OpenGLRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    // Skip if shadows are disabled
    if (shadowQuality == 0 || shadowMapFBO == 0)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // IMPORTANT: Set view matrix from camera FIRST before calculating cascade matrices
    // This must happen before getLightSpaceMatrixForCascade() which uses view_matrix
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAt(pos, target, up);

    // Calculate cascade splits
    calculateCascadeSplits(0.1f, 1000.0f);

    // Calculate light space matrices for each cascade
    float aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    for (int i = 0; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }

    // Keep legacy matrix updated (use first cascade)
    lightSpaceMatrix = lightSpaceMatrices[0];

    // Set up shadow pass state
    glViewport(0, 0, currentShadowSize, currentShadowSize);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);

    currentCascade = 0;
    lighting_ubo_dirty = true;
    global_uniforms_dirty = true;
}

// Begin rendering a specific cascade
void OpenGLRenderAPI::beginCascade(int cascadeIndex)
{
    currentCascade = cascadeIndex;
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowMapTextureArray, 0, cascadeIndex);
    glClear(GL_DEPTH_BUFFER_BIT);
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
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);
}

glm::mat4 OpenGLRenderAPI::getLightSpaceMatrix()
{
    return lightSpaceMatrix;
}

int OpenGLRenderAPI::getCascadeCount() const
{
    return NUM_CASCADES;
}

const float* OpenGLRenderAPI::getCascadeSplitDistances() const
{
    return cascadeSplitDistances;
}

const glm::mat4* OpenGLRenderAPI::getLightSpaceMatrices() const
{
    return lightSpaceMatrices;
}

void OpenGLRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded to GPU (lazy upload)
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    // For indexed meshes, draw_count is the index count; for non-indexed, it's vertex count
    GLsizei draw_count = m.gpu_mesh->isIndexed()
        ? static_cast<GLsizei>(m.gpu_mesh->getIndexCount())
        : static_cast<GLsizei>(m.gpu_mesh->getVertexCount());

    renderMeshInternal(m, 0, draw_count, state);
}

void OpenGLRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0) return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len)
    {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded to GPU (lazy upload)
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    renderMeshInternal(m, static_cast<GLint>(start_vertex), static_cast<GLsizei>(vertex_count), state);
}

void OpenGLRenderAPI::renderMeshInternal(const mesh& m, GLint start_vertex, GLsizei draw_count, const RenderState& state)
{
    // Get appropriate shader
    Shader* shader = in_shadow_pass
        ? shader_manager->getShader("shadow")
        : getShaderForRenderState(state);

    if (!shader || !shader->isValid())
        return;

    // Bind shader (tracked to avoid redundant switches)
    bool shader_changed = false;
    if (current_shader_id != shader->getProgramID())
    {
        shader->use();
        current_shader_id = shader->getProgramID();
        shader_changed = true;
    }

    if (in_shadow_pass)
    {
        // Shadow pass: only needs per-cascade light matrix + model matrix
        if (shader_changed || global_uniforms_dirty)
        {
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrices[currentCascade]);
            global_uniforms_dirty = false;
        }
        shader->setUniform("uModel", current_model_matrix);
    }
    else
    {
        // Main pass: global uniforms are in UBOs, only set per-object uniforms
        if (shader_changed || global_uniforms_dirty)
        {
            // Shadow map array sampler binding (texture unit 1 already bound in beginFrame)
            shader->setUniform("uShadowMapArray", 1);
            global_uniforms_dirty = false;
        }

        // Per-object uniforms
        shader->setUniform("uModel", current_model_matrix);
        shader->setUniform("uColor", state.color);

        // Texture binding
        if (!m.uses_material_ranges && m.texture_set && m.texture != INVALID_TEXTURE)
        {
            bindTexture(m.texture);
            shader->setUniform("uTexture", 0);
            shader->setUniform("uUseTexture", true);
        }
        else if (m.uses_material_ranges)
        {
            // Multi-material: texture already bound by renderer.hpp
            shader->setUniform("uTexture", 0);
            shader->setUniform("uUseTexture", true);
        }
        else
        {
            shader->setUniform("uUseTexture", false);
        }
    }

    // Apply render state
    applyRenderState(state);

    // Draw
    OpenGLMesh* glMesh = dynamic_cast<OpenGLMesh*>(m.gpu_mesh);
    if (glMesh)
    {
        glMesh->bind();
        if (glMesh->isIndexed())
        {
            glDrawElements(GL_TRIANGLES, draw_count, GL_UNSIGNED_INT,
                           (void*)(start_vertex * sizeof(uint32_t)));
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, start_vertex, draw_count);
        }
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

void OpenGLRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    // Store lighting parameters for shader uniforms
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);

    lighting_ubo_dirty = true;
    global_uniforms_dirty = true;
}

void OpenGLRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

glm::mat4 OpenGLRenderAPI::getProjectionMatrix() const
{
    return projection_matrix;
}

glm::mat4 OpenGLRenderAPI::getViewMatrix() const
{
    return view_matrix;
}

void OpenGLRenderAPI::renderSkybox()
{
    if (skybox)
    {
        // Restore depth state in case depth prepass left it as GL_EQUAL / no-write
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        current_gpu_state.depth_test = DepthTest::LessEqual;
        current_gpu_state.depth_write = true;

        // Sun direction is opposite of light direction (direction TO the sun)
        glm::vec3 sunDir = -current_light_direction;

        skybox->render(view_matrix, projection_matrix, sunDir);

        // Skybox uses its own shader - invalidate our tracker
        Shader* sky_shader = shader_manager->getShader("sky");
        current_shader_id = sky_shader ? sky_shader->getProgramID() : 0;

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

IGPUMesh* OpenGLRenderAPI::createMesh()
{
    return new OpenGLMesh();
}

void OpenGLRenderAPI::beginDepthPrepass()
{
    Shader* depth_shader = shader_manager->getShader("depth");
    if (!depth_shader || !depth_shader->isValid()) return;

    depth_shader->use();
    current_shader_id = depth_shader->getProgramID();

    // Depth prepass: write depth only, no color
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
}

void OpenGLRenderAPI::endDepthPrepass()
{
    // Restore color writes
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Main pass uses EQUAL depth test - only shade fragments matching depth buffer
    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    current_gpu_state.depth_test = DepthTest::LessEqual; // approximate
    current_gpu_state.depth_write = false;
}

void OpenGLRenderAPI::renderMeshDepthOnly(const mesh& m)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    Shader* depth_shader = shader_manager->getShader("depth");
    if (!depth_shader) return;

    // Only set per-object model matrix
    depth_shader->setUniform("uModel", current_model_matrix);

    // Apply culling
    RenderState state = m.getRenderState();
    if (state.cull_mode != current_gpu_state.cull_mode)
    {
        if (state.cull_mode == CullMode::None)
            glDisable(GL_CULL_FACE);
        else
        {
            if (current_gpu_state.cull_mode == CullMode::None)
                glEnable(GL_CULL_FACE);
            glCullFace(getGLCullMode(state.cull_mode));
        }
        current_gpu_state.cull_mode = state.cull_mode;
    }

    // Draw
    OpenGLMesh* glMesh = dynamic_cast<OpenGLMesh*>(m.gpu_mesh);
    if (glMesh)
    {
        glMesh->bind();
        if (glMesh->isIndexed())
        {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(glMesh->getIndexCount()),
                           GL_UNSIGNED_INT, nullptr);
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(glMesh->getVertexCount()));
        }
        glMesh->unbind();
    }
}

void OpenGLRenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || !shader_manager) return;

    // Use unlit shader for debug lines
    Shader* shader = shader_manager->getShader("unlit");
    if (!shader || !shader->isValid()) return;

    shader->use();
    // View/Projection are in UBO binding 0 (CameraData) - already uploaded
    shader->setUniform("uModel", glm::mat4(1.0f));
    shader->setUniform("uColor", glm::vec3(1.0f));

    // Unbind any texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create debug VAO/VBO with persistent mapping
    size_t data_size = vertex_count * sizeof(vertex);

    if (debug_vao == 0)
    {
        glGenVertexArrays(1, &debug_vao);
        glBindVertexArray(debug_vao);

        glGenBuffers(1, &debug_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, debug_vbo);

        // Allocate with persistent mapping - start with 64KB or what we need
        debug_vbo_capacity = std::max(data_size, (size_t)(64 * 1024));
        glBufferStorage(GL_ARRAY_BUFFER, debug_vbo_capacity, nullptr,
                        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        debug_vbo_mapped_ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, debug_vbo_capacity,
                        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

        // Position (location 0)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)0);
        glEnableVertexAttribArray(0);
        // Normal - per-vertex color (location 1)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // TexCoord (location 2)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }

    // Resize if needed (rare - only when debug data exceeds 64KB)
    if (data_size > debug_vbo_capacity)
    {
        glBindBuffer(GL_ARRAY_BUFFER, debug_vbo);
        if (debug_vbo_mapped_ptr)
            glUnmapBuffer(GL_ARRAY_BUFFER);

        glDeleteBuffers(1, &debug_vbo);
        glGenBuffers(1, &debug_vbo);
        glBindVertexArray(debug_vao);
        glBindBuffer(GL_ARRAY_BUFFER, debug_vbo);

        debug_vbo_capacity = data_size * 2;
        glBufferStorage(GL_ARRAY_BUFFER, debug_vbo_capacity, nullptr,
                        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        debug_vbo_mapped_ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, debug_vbo_capacity,
                        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

        // Re-setup vertex attribs
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }

    // Write directly to persistently mapped buffer (no glBufferSubData needed)
    if (debug_vbo_mapped_ptr)
    {
        std::memcpy(debug_vbo_mapped_ptr, vertices, data_size);
    }

    glBindVertexArray(debug_vao);

    // Disable depth write but keep depth test so lines render behind geometry correctly
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertex_count));

    // Restore state
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);

    glBindVertexArray(0);

    // Restore shader tracking
    current_shader_id = 0;
    global_uniforms_dirty = true;
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

// Graphics settings implementation
void OpenGLRenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool OpenGLRenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}

void OpenGLRenderAPI::setShadowQuality(int quality)
{
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;

    if (quality == shadowQuality) return;

    shadowQuality = quality;

    // Map quality to resolution: 0=Off(0), 1=Low(1024), 2=Medium(2048), 3=High(4096)
    unsigned int newSize = 0;
    switch (quality)
    {
    case 0: newSize = 0; break;     // Off - no shadow map
    case 1: newSize = 1024; break;  // Low
    case 2: newSize = 2048; break;  // Medium
    case 3: newSize = 4096; break;  // High
    }

    if (newSize != currentShadowSize)
    {
        recreateShadowMapResources(newSize);
    }
}

int OpenGLRenderAPI::getShadowQuality() const
{
    return shadowQuality;
}

void OpenGLRenderAPI::recreateShadowMapResources(unsigned int size)
{
    // Delete existing resources
    if (shadowMapFBO)
    {
        glDeleteFramebuffers(1, &shadowMapFBO);
        shadowMapFBO = 0;
    }
    if (shadowMapTextureArray)
    {
        glDeleteTextures(1, &shadowMapTextureArray);
        shadowMapTextureArray = 0;
    }

    currentShadowSize = size;

    // If size is 0, shadows are disabled - don't create resources
    if (size == 0)
    {
        LOG_ENGINE_INFO("Shadows disabled");
        return;
    }

    // Recreate FBO and texture array at new size
    glGenFramebuffers(1, &shadowMapFBO);
    glGenTextures(1, &shadowMapTextureArray);

    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F,
                 size, size, NUM_CASCADES,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowMapTextureArray, 0, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ENGINE_ERROR("Failed to recreate shadow FBO at size {}", size);
    }
    else
    {
        LOG_ENGINE_INFO("Shadow map resized to {}x{}", size, size);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}