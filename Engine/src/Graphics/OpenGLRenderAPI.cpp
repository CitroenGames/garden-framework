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
    shader_manager = new ShaderManager();
    shader_manager->createDefaultShaders();
    
    // Load FXAA shader
    shader_manager->loadShader("fxaa", "assets/shaders/fxaa.vert", "assets/shaders/fxaa.frag");
    
    // Load Sky shader
    shader_manager->loadShader("sky", "assets/shaders/sky.vert", "assets/shaders/sky.frag");

    // Load Shadow shader
    shader_manager->loadShader("shadow", "assets/shaders/shadow.vert", "assets/shaders/shadow.frag");

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
    post_processing = new PostProcessing();
    if (!post_processing->initialize(width, height, shader_manager->getShader("fxaa")))
    {
        printf("Failed to initialize PostProcessing\n");
    }

    // Initialize Skybox
    skybox = new Skybox();
    if (!skybox->initialize(shader_manager->getShader("sky")))
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

    if (shadowMapTextureArray)
    {
        glDeleteTextures(1, &shadowMapTextureArray);
        shadowMapTextureArray = 0;
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
        1000.0f
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
            // Use current cascade's light space matrix
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrices[currentCascade]);
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

            // Set CSM uniforms - all cascade matrices and split distances
            for (int i = 0; i < NUM_CASCADES; i++) {
                shader->setUniform("uLightSpaceMatrices[" + std::to_string(i) + "]", lightSpaceMatrices[i]);
            }
            for (int i = 0; i <= NUM_CASCADES; i++) {
                shader->setUniform("uCascadeSplits[" + std::to_string(i) + "]", cascadeSplitDistances[i]);
            }
            shader->setUniform("uCascadeCount", NUM_CASCADES);
            shader->setUniform("uDebugCascades", debugCascades);

            static bool csm_logged = false;
            if (!csm_logged) {
                LOG_ENGINE_INFO("CSM uniforms set: cascadeCount={}, debugCascades={}", NUM_CASCADES, debugCascades);
                csm_logged = true;
            }

            // Set lighting uniforms
            shader->setUniform("uLightDir", current_light_direction);
            shader->setUniform("uLightAmbient", current_light_ambient);
            shader->setUniform("uLightDiffuse", current_light_diffuse);

            // Bind Shadow Map Array (Unit 1)
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);
            shader->setUniform("uShadowMapArray", 1);

            global_uniforms_dirty = false;
        }

        // Set object specific uniforms
        shader->setUniform("uModel", current_model_matrix);
        shader->setUniform("uColor", state.color);

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
            // Use current cascade's light space matrix
            shader->setUniform("uLightSpaceMatrix", lightSpaceMatrices[currentCascade]);
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

            // Set CSM uniforms - all cascade matrices and split distances
            for (int i = 0; i < NUM_CASCADES; i++) {
                shader->setUniform("uLightSpaceMatrices[" + std::to_string(i) + "]", lightSpaceMatrices[i]);
            }
            for (int i = 0; i <= NUM_CASCADES; i++) {
                shader->setUniform("uCascadeSplits[" + std::to_string(i) + "]", cascadeSplitDistances[i]);
            }
            shader->setUniform("uCascadeCount", NUM_CASCADES);
            shader->setUniform("uDebugCascades", debugCascades);

            static bool csm_logged = false;
            if (!csm_logged) {
                LOG_ENGINE_INFO("CSM uniforms set: cascadeCount={}, debugCascades={}", NUM_CASCADES, debugCascades);
                csm_logged = true;
            }

            // Set lighting uniforms
            shader->setUniform("uLightDir", current_light_direction);
            shader->setUniform("uLightAmbient", current_light_ambient);
            shader->setUniform("uLightDiffuse", current_light_diffuse);

            // Bind Shadow Map Array (Unit 1)
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTextureArray);
            shader->setUniform("uShadowMapArray", 1);

            global_uniforms_dirty = false;
        }

        // Set object specific uniforms
        shader->setUniform("uModel", current_model_matrix);
        shader->setUniform("uColor", state.color);

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

void OpenGLRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    // Store lighting parameters for shader uniforms
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);

    global_uniforms_dirty = true;
}

void OpenGLRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

void OpenGLRenderAPI::renderSkybox()
{
    if (skybox)
    {
        // Sun direction is opposite of light direction (direction TO the sun)
        glm::vec3 sunDir = -current_light_direction;

        skybox->render(view_matrix, projection_matrix, sunDir);

        // Skybox modifies GL state, so we need to sync our trackers
        current_shader_id = 0;

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