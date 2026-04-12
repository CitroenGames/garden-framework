#pragma once

#include "MetalTypes.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/Log.hpp"

// ============================================================================
// Implementation struct (Pimpl) — shared by all MetalRenderAPI_*.mm files
// ============================================================================
struct MetalRenderAPIImpl {
    // Core Metal objects
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    CAMetalLayer* metalLayer = nil;
    id<MTLLibrary> shaderLibrary = nil;

    // Per-frame
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
    id<MTLCommandBuffer> commandBuffer = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    id<CAMetalDrawable> currentDrawable = nil;
    dispatch_semaphore_t frameSemaphore;
    uint32_t currentFrame = 0;

    // Depth
    id<MTLTexture> depthTexture = nil;

    // Pipeline states
    id<MTLRenderPipelineState> basicPipeline = nil;
    id<MTLRenderPipelineState> basicPipelineAlpha = nil;
    id<MTLRenderPipelineState> basicPipelineAdditive = nil;
    id<MTLRenderPipelineState> shadowPipeline = nil;
    id<MTLRenderPipelineState> skyboxPipeline = nil;
    id<MTLRenderPipelineState> fxaaPipeline = nil;

    // Depth stencil states
    id<MTLDepthStencilState> depthLessEqual = nil;
    id<MTLDepthStencilState> depthLessEqualNoWrite = nil;
    id<MTLDepthStencilState> depthNone = nil;
    id<MTLDepthStencilState> shadowDepthState = nil;

    // Skybox
    id<MTLBuffer> skyboxVertexBuffer = nil;

    // FXAA
    id<MTLBuffer> fxaaVertexBuffer = nil;
    id<MTLTexture> offscreenTexture = nil;
    id<MTLTexture> offscreenDepthTexture = nil;
    bool fxaaEnabled = true;
    bool fxaaInitialized = false;

    // Editor viewport render target
    id<MTLTexture> viewportTexture = nil;
    id<MTLTexture> viewportDepthTexture = nil;
    int viewportWidthRT = 0;
    int viewportHeightRT = 0;

    // Preview render target (asset preview panel)
    id<MTLTexture> previewTexture = nil;
    id<MTLTexture> previewDepthTexture = nil;
    int previewWidthRT = 0;
    int previewHeightRT = 0;

    // PIE viewport render targets (for multi-player Play-In-Editor)
    struct PIEViewportTarget {
        id<MTLTexture> colorTexture = nil;
        id<MTLTexture> depthTexture = nil;
        // Offscreen texture for FXAA intermediate rendering
        id<MTLTexture> offscreenTexture = nil;
        id<MTLTexture> offscreenDepthTexture = nil;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<int, PIEViewportTarget> pieViewports;
    int nextPIEId = 0;
    int activeSceneTarget = -1; // -1 = main viewport

    // Shadow mapping
    static constexpr int NUM_CASCADES = 4;
    id<MTLTexture> shadowMapArray = nil;
    id<MTLSamplerState> shadowSampler = nil;
    uint32_t shadowMapSize = 4096;
    int shadowQuality = 3;
    bool inShadowPass = false;
    int currentCascade = 0;
    float cascadeSplitDistances[5] = { 0.1f, 10.0f, 35.0f, 90.0f, 200.0f };
    float cascadeSplitLambda = 0.92f;
    glm::mat4 lightSpaceMatrices[4];

    // Texture management
    std::unordered_map<TextureHandle, MetalTexture> textures;
    TextureHandle nextTextureHandle = 1;
    TextureHandle boundTexture = INVALID_TEXTURE;
    id<MTLTexture> defaultTexture = nil;
    id<MTLSamplerState> defaultSampler = nil;

    // Matrix stack
    glm::mat4 projectionMatrix = glm::mat4(1.0f);
    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 currentModelMatrix = glm::mat4(1.0f);
    std::stack<glm::mat4> modelMatrixStack;

    // Lighting
    glm::vec3 lightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 lightAmbient = glm::vec3(0.2f);
    glm::vec3 lightDiffuse = glm::vec3(0.8f);
    bool lightingEnabled = true;
    LightCBuffer currentLights{};

    // Window state
    WindowHandle windowHandle = nullptr;
    int viewportWidth = 0;
    int viewportHeight = 0;
    float fieldOfView = 75.0f;

    // Render state
    RenderState currentState;
    bool frameStarted = false;
    glm::vec3 clearColor = glm::vec3(0.2f, 0.3f, 0.8f);

    // Main pass state
    bool mainPassActive = false;

    // ImGui render pass descriptor (stored for ImGui rendering)
    MTLRenderPassDescriptor* imguiRenderPassDesc = nil;

    // Redundant bind tracking
    id<MTLRenderPipelineState> lastBoundPipeline = nil;
    id<MTLDepthStencilState> lastBoundDepthStencil = nil;
    id<MTLBuffer> lastBoundVertexBuffer = nil;
    TextureHandle lastBoundTextureHandle = INVALID_TEXTURE;
    MTLCullMode lastCullMode = MTLCullModeBack;
    bool shadowMapBound = false;

    // Per-frame UBO cache (built once, per-draw fields overwritten)
    MetalGlobalUBO cachedPerFrameUBO{};
    bool perFrameUBOReady = false;

    // Draw call counter for diagnostics
    uint32_t drawCallCount = 0;
    uint32_t frameNumber = 0;

    // GPU error tracking for auto-recovery
    uint32_t gpuErrorCount = 0;
    static constexpr uint32_t MAX_GPU_ERRORS_BEFORE_RECOVERY = 5;

    // ========================================================================
    // Inline helper methods (small, called from multiple files)
    // ========================================================================

    void updatePerFrameUBO()
    {
        if (perFrameUBOReady) return;
        cachedPerFrameUBO = {};
        cachedPerFrameUBO.view = viewMatrix;
        cachedPerFrameUBO.projection = projectionMatrix;
        for (int i = 0; i < NUM_CASCADES; i++)
            cachedPerFrameUBO.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        cachedPerFrameUBO.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                                     cascadeSplitDistances[2], cascadeSplitDistances[3]);
        cachedPerFrameUBO.cascadeSplit4 = cascadeSplitDistances[4];
        cachedPerFrameUBO.lightDir = lightDirection;
        cachedPerFrameUBO.lightAmbient = lightAmbient;
        cachedPerFrameUBO.cascadeCount = (shadowQuality > 0) ? NUM_CASCADES : 0;
        cachedPerFrameUBO.lightDiffuse = lightDiffuse;
        cachedPerFrameUBO.debugCascades = 0;
        perFrameUBOReady = true;
    }

    id<MTLTexture> createDepthTextureWithSize(uint32_t w, uint32_t h)
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModePrivate;
        return [device newTextureWithDescriptor:desc];
    }

    // Create command buffer only (drawable acquired separately, as late as possible)
    bool ensureCommandBuffer()
    {
        if (commandBuffer) return true;

        // Wait for a free frame slot
        dispatch_semaphore_wait(frameSemaphore, DISPATCH_TIME_FOREVER);

        // Create command buffer with enhanced error reporting
        MTLCommandBufferDescriptor* desc = [[MTLCommandBufferDescriptor alloc] init];
        desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        commandBuffer = [commandQueue commandBufferWithDescriptor:desc];
        if (!commandBuffer) {
            printf("[Metal] Failed to create command buffer\n");
            dispatch_semaphore_signal(frameSemaphore);
            return false;
        }
        commandBuffer.label = @"Frame Command Buffer";
        return true;
    }

    // Acquire drawable as late as possible to minimize compositor stalls
    bool ensureDrawable()
    {
        if (currentDrawable) return true;

        currentDrawable = [metalLayer nextDrawable];
        if (!currentDrawable) {
            printf("[Metal] Failed to acquire drawable\n");
            return false;
        }
        return true;
    }

    // ========================================================================
    // Declared helper methods (defined in corresponding .mm files)
    // ========================================================================

    // Defined in MetalRenderAPI.mm (init)
    bool loadShaderLibrary();
    bool createPipelines();
    void createDepthStencilStates();
    void createDefaultTexture();

    // Defined in MetalRenderAPI_Shadow.mm
    void createShadowResources();
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                             const glm::mat4& viewMat, float fov, float aspect);
    void recreateShadowResources(uint32_t size);

    // Defined in MetalRenderAPI_Viewport.mm
    void createOffscreenResources(int w, int h);
    void createOffscreenResources();
    void createViewportResources(int w, int h);
    void createPIEViewportTextures(PIEViewportTarget& target, int w, int h);
};
