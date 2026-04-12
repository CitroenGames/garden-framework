#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include "MetalMesh.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Vertex.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"

// ============================================================================
// MetalRenderAPIImpl helper definitions (init-related)
// ============================================================================

bool MetalRenderAPIImpl::loadShaderLibrary()
{
    NSError* error = nil;

    // Try loading precompiled metallib
    NSString* libPath = [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/shaders.metallib").c_str()];
    NSURL* libURL = [NSURL fileURLWithPath:libPath];

    if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
        shaderLibrary = [device newLibraryWithURL:libURL error:&error];
        if (shaderLibrary) {
            LOG_ENGINE_INFO("[Metal] Loaded precompiled shader library");
            return true;
        }
        LOG_ENGINE_WARN("[Metal] Failed to load metallib: {}", [[error localizedDescription] UTF8String]);
    }

    // Fallback: compile from source
    // Read common.metal first (shared types/functions), then individual shader files
    NSArray<NSString*>* shaderFiles = @[
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/common.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/basic.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/shadow.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/sky.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/fxaa.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/skinned.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/skinned_shadow.metal").c_str()]];

    NSMutableString* allSource = [NSMutableString string];
    // Add common header once
    [allSource appendString:@"#include <metal_stdlib>\nusing namespace metal;\n\n"];

    for (NSString* path in shaderFiles) {
        NSString* source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
        if (source) {
            // Strip the #include and using namespace lines from individual files
            NSString* stripped = [source stringByReplacingOccurrencesOfString:@"#include <metal_stdlib>" withString:@""];
            stripped = [stripped stringByReplacingOccurrencesOfString:@"using namespace metal;" withString:@""];
            [allSource appendFormat:@"\n// === %@ ===\n%@\n", path, stripped];
        } else {
            LOG_ENGINE_WARN("[Metal] Could not read shader file: {}", [path UTF8String]);
        }
    }

    if ([allSource length] == 0) {
        LOG_ENGINE_FATAL("[Metal] No shader sources found");
        return false;
    }

    MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
    shaderLibrary = [device newLibraryWithSource:allSource options:opts error:&error];
    if (!shaderLibrary) {
        LOG_ENGINE_FATAL("[Metal] Shader compilation failed: {}", [[error localizedDescription] UTF8String]);
        return false;
    }

    LOG_ENGINE_INFO("[Metal] Compiled shaders from source");
    return true;
}

bool MetalRenderAPIImpl::createPipelines()
{
    // Vertex descriptor for mesh vertices (pos, normal, texcoord = 32 bytes)
    MTLVertexDescriptor* vertexDesc = [[MTLVertexDescriptor alloc] init];
    // Position: float3 at offset 0
    vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
    vertexDesc.attributes[0].offset = offsetof(vertex, vx);
    vertexDesc.attributes[0].bufferIndex = 0;
    // Normal: float3 at offset 12
    vertexDesc.attributes[1].format = MTLVertexFormatFloat3;
    vertexDesc.attributes[1].offset = offsetof(vertex, nx);
    vertexDesc.attributes[1].bufferIndex = 0;
    // TexCoord: float2 at offset 24
    vertexDesc.attributes[2].format = MTLVertexFormatFloat2;
    vertexDesc.attributes[2].offset = offsetof(vertex, u);
    vertexDesc.attributes[2].bufferIndex = 0;
    // Layout
    vertexDesc.layouts[0].stride = sizeof(vertex);
    vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    NSError* error = nil;

    // --- Basic pipeline (no blend) ---
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"basic_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"basic_fragment"];
        desc.vertexDescriptor = vertexDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = NO;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        desc.label = @"Basic Pipeline (No Blend)";

        if (!desc.vertexFunction || !desc.fragmentFunction) {
            printf("[Metal] Failed to find basic shader functions\n");
            return false;
        }

        basicPipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!basicPipeline) {
            printf("[Metal] Failed to create basic pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }
        printf("[Metal] Pipeline created: Basic (No Blend)\n");
    }

    // --- Basic pipeline (alpha blend) ---
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"basic_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"basic_fragment"];
        desc.vertexDescriptor = vertexDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        desc.label = @"Basic Pipeline (Alpha)";

        basicPipelineAlpha = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!basicPipelineAlpha) {
            printf("[Metal] Failed to create alpha pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }
        printf("[Metal] Pipeline created: Basic (Alpha Blend)\n");
    }

    // --- Basic pipeline (additive blend) ---
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"basic_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"basic_fragment"];
        desc.vertexDescriptor = vertexDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        desc.label = @"Basic Pipeline (Additive)";

        basicPipelineAdditive = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!basicPipelineAdditive) {
            printf("[Metal] Failed to create additive pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }
        printf("[Metal] Pipeline created: Basic (Additive Blend)\n");
    }

    // --- Shadow pipeline ---
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"shadow_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"shadow_fragment"];
        desc.vertexDescriptor = vertexDesc;
        // No color attachment for shadow pass (depth-only)
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        desc.label = @"Shadow Pipeline";

        if (!desc.vertexFunction) {
            printf("[Metal] Warning: shadow_vertex not found, shadows disabled\n");
        } else {
            shadowPipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            if (!shadowPipeline) {
                printf("[Metal] Failed to create shadow pipeline: %s\n", [[error localizedDescription] UTF8String]);
            } else {
                printf("[Metal] Pipeline created: Shadow\n");
            }
        }
    }

    // --- Skybox pipeline ---
    {
        MTLVertexDescriptor* skyDesc = [[MTLVertexDescriptor alloc] init];
        skyDesc.attributes[0].format = MTLVertexFormatFloat3;
        skyDesc.attributes[0].offset = 0;
        skyDesc.attributes[0].bufferIndex = 0;
        skyDesc.layouts[0].stride = sizeof(float) * 3;
        skyDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"sky_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"sky_fragment"];
        desc.vertexDescriptor = skyDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        desc.label = @"Skybox Pipeline";

        if (desc.vertexFunction && desc.fragmentFunction) {
            skyboxPipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            if (!skyboxPipeline) {
                printf("[Metal] Failed to create skybox pipeline: %s\n", [[error localizedDescription] UTF8String]);
            } else {
                printf("[Metal] Pipeline created: Skybox\n");
            }
        }
    }

    // --- FXAA pipeline ---
    {
        MTLVertexDescriptor* fxaaDesc = [[MTLVertexDescriptor alloc] init];
        fxaaDesc.attributes[0].format = MTLVertexFormatFloat2;
        fxaaDesc.attributes[0].offset = offsetof(FXAAVertex, x);
        fxaaDesc.attributes[0].bufferIndex = 0;
        fxaaDesc.attributes[1].format = MTLVertexFormatFloat2;
        fxaaDesc.attributes[1].offset = offsetof(FXAAVertex, u);
        fxaaDesc.attributes[1].bufferIndex = 0;
        fxaaDesc.layouts[0].stride = sizeof(FXAAVertex);
        fxaaDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.rasterSampleCount = 1;
        desc.vertexFunction = [shaderLibrary newFunctionWithName:@"fxaa_vertex"];
        desc.fragmentFunction = [shaderLibrary newFunctionWithName:@"fxaa_fragment"];
        desc.vertexDescriptor = fxaaDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.label = @"FXAA Pipeline";

        if (desc.vertexFunction && desc.fragmentFunction) {
            fxaaPipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            if (!fxaaPipeline) {
                printf("[Metal] Failed to create FXAA pipeline: %s\n", [[error localizedDescription] UTF8String]);
            } else {
                printf("[Metal] Pipeline created: FXAA\n");
            }
            fxaaInitialized = (fxaaPipeline != nil);
        }
    }

    return true;
}

void MetalRenderAPIImpl::createDepthStencilStates()
{
    // LessEqual, write enabled
    {
        MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
        desc.depthCompareFunction = MTLCompareFunctionLessEqual;
        desc.depthWriteEnabled = YES;
        depthLessEqual = [device newDepthStencilStateWithDescriptor:desc];
    }
    // LessEqual, no write
    {
        MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
        desc.depthCompareFunction = MTLCompareFunctionLessEqual;
        desc.depthWriteEnabled = NO;
        depthLessEqualNoWrite = [device newDepthStencilStateWithDescriptor:desc];
    }
    // No depth test
    {
        MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
        desc.depthCompareFunction = MTLCompareFunctionAlways;
        desc.depthWriteEnabled = NO;
        depthNone = [device newDepthStencilStateWithDescriptor:desc];
    }
    // Shadow depth state
    {
        MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
        desc.depthCompareFunction = MTLCompareFunctionLessEqual;
        desc.depthWriteEnabled = YES;
        shadowDepthState = [device newDepthStencilStateWithDescriptor:desc];
    }
}

void MetalRenderAPIImpl::createDefaultTexture()
{
    // 1x1 white texture
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:1
                                                                                  height:1
                                                                               mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    defaultTexture = [device newTextureWithDescriptor:desc];

    uint8_t white[] = {255, 255, 255, 255};
    [defaultTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];

    // Default sampler
    MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
    sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.mipFilter = MTLSamplerMipFilterLinear;
    sampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.maxAnisotropy = 16;
    defaultSampler = [device newSamplerStateWithDescriptor:sampDesc];
}

// ============================================================================
// MetalRenderAPI implementation — core lifecycle
// ============================================================================

MetalRenderAPI::MetalRenderAPI()
    : impl(std::make_unique<MetalRenderAPIImpl>())
{
}

MetalRenderAPI::~MetalRenderAPI()
{
    shutdown();
}

bool MetalRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    impl->windowHandle = window;
    impl->viewportWidth = width;
    impl->viewportHeight = height;
    impl->fieldOfView = fov;

    // Create Metal device
    impl->device = MTLCreateSystemDefaultDevice();
    if (!impl->device) {
        LOG_ENGINE_FATAL("[Metal] Failed to create Metal device");
        return false;
    }
    LOG_ENGINE_INFO("[Metal] Using device: {}", [[impl->device name] UTF8String]);
    LOG_ENGINE_INFO("[Metal] GPU Family: Apple {}",
           [impl->device supportsFamily:MTLGPUFamilyApple7] ? "7+" :
           [impl->device supportsFamily:MTLGPUFamilyApple6] ? "6+" :
           [impl->device supportsFamily:MTLGPUFamilyApple5] ? "5+" : "< 5");
    LOG_ENGINE_INFO("[Metal] Max buffer length: {} MB", (unsigned long)([impl->device maxBufferLength] / (1024 * 1024)));
    LOG_ENGINE_INFO("[Metal] Recommended max working set: {} MB", (unsigned long)([impl->device recommendedMaxWorkingSetSize] / (1024 * 1024)));

    // Create command queue
    impl->commandQueue = [impl->device newCommandQueue];
    if (!impl->commandQueue) {
        LOG_ENGINE_FATAL("[Metal] Failed to create command queue");
        return false;
    }

    // Create frame semaphore
    impl->frameSemaphore = dispatch_semaphore_create(MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT);

    // Set up CAMetalLayer on the SDL window
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nsWindow) {
        LOG_ENGINE_FATAL("[Metal] Failed to get window info: {}", SDL_GetError());
        return false;
    }
    NSView* contentView = [nsWindow contentView];
    [contentView setWantsLayer:YES];

    impl->metalLayer = [CAMetalLayer layer];
    impl->metalLayer.device = impl->device;
    impl->metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl->metalLayer.framebufferOnly = YES; // Drawable is only written to (FXAA uses offscreenTexture)
    impl->metalLayer.drawableSize = CGSizeMake(width, height);
    [contentView setLayer:impl->metalLayer];

    // Create depth texture
    impl->depthTexture = impl->createDepthTextureWithSize(width, height);
    if (!impl->depthTexture) {
        LOG_ENGINE_FATAL("[Metal] Failed to create depth texture");
        return false;
    }

    // Load shaders
    if (!impl->loadShaderLibrary()) {
        LOG_ENGINE_FATAL("[Metal] Failed to load shader library");
        return false;
    }

    // Create pipeline states
    if (!impl->createPipelines()) {
        LOG_ENGINE_FATAL("[Metal] Failed to create pipelines");
        return false;
    }

    // Create depth stencil states
    impl->createDepthStencilStates();

    // Create shadow resources
    impl->createShadowResources();

    // Create default texture and sampler
    impl->createDefaultTexture();

    // Create skybox vertex buffer
    impl->skyboxVertexBuffer = [impl->device newBufferWithBytes:skyboxVertices
                                                         length:sizeof(skyboxVertices)
                                                        options:MTLResourceStorageModeShared];

    // Create FXAA vertex buffer
    impl->fxaaVertexBuffer = [impl->device newBufferWithBytes:fxaaQuadVertices
                                                       length:sizeof(fxaaQuadVertices)
                                                      options:MTLResourceStorageModeShared];

    // Create offscreen resources for FXAA
    impl->createOffscreenResources();

    // Initialize projection matrix
    float ratio = (float)width / (float)height;
    impl->projectionMatrix = glm::perspectiveRH_ZO(glm::radians(fov), ratio, 0.1f, 1000.0f);

    LOG_ENGINE_INFO("[Metal] Shadow map: {}x{} ({} cascades, quality={})",
           impl->shadowMapSize, impl->shadowMapSize, MetalRenderAPIImpl::NUM_CASCADES, impl->shadowQuality);
    LOG_ENGINE_INFO("[Metal] FXAA: {}", impl->fxaaInitialized ? "enabled" : "disabled");
    LOG_ENGINE_INFO("[Metal] Render API initialized ({}x{}, FOV: {:.1f})", width, height, fov);
    return true;
}

void MetalRenderAPI::shutdown()
{
    if (!impl->device) return;

    // Stop accepting new frames
    impl->frameStarted = false;

    // Wait for GPU to finish all in-flight frames via semaphore drain
    // (this is the correct synchronization — commandBuffer may already be nil after endFrame)
    for (int i = 0; i < MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT; i++) {
        dispatch_semaphore_wait(impl->frameSemaphore, DISPATCH_TIME_FOREVER);
    }
    for (int i = 0; i < MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT; i++) {
        dispatch_semaphore_signal(impl->frameSemaphore);
    }

    // Now safe to release — all GPU work is complete
    impl->commandBuffer = nil;
    impl->encoder = nil;
    impl->currentDrawable = nil;
    impl->mainPassActive = false;

    // Release all textures
    impl->textures.clear();

    // Release PIE viewport resources
    impl->pieViewports.clear();
    impl->activeSceneTarget = -1;

    // Nil out Metal objects (ARC handles cleanup)
    impl->basicPipeline = nil;
    impl->basicPipelineAlpha = nil;
    impl->basicPipelineAdditive = nil;
    impl->shadowPipeline = nil;
    impl->skyboxPipeline = nil;
    impl->fxaaPipeline = nil;
    impl->depthTexture = nil;
    impl->shadowMapArray = nil;
    impl->offscreenTexture = nil;
    impl->offscreenDepthTexture = nil;
    impl->viewportTexture = nil;
    impl->viewportDepthTexture = nil;
    impl->skyboxVertexBuffer = nil;
    impl->fxaaVertexBuffer = nil;
    impl->defaultTexture = nil;
    impl->defaultSampler = nil;
    impl->shaderLibrary = nil;
    impl->commandQueue = nil;
    impl->metalLayer = nil;
    impl->device = nil;

    printf("[Metal] Render API shut down\n");
}

// ============================================================================
// Resource creation
// ============================================================================

IGPUMesh* MetalRenderAPI::createMesh()
{
    MetalMesh* metalMesh = new MetalMesh();
    metalMesh->setDevice((__bridge void*)impl->device);
    metalMesh->setCommandQueue((__bridge void*)impl->commandQueue);
    return metalMesh;
}

// ============================================================================
// Metal-specific accessors for ImGui
// ============================================================================

void* MetalRenderAPI::getDevice() const
{
    return (__bridge void*)impl->device;
}

void* MetalRenderAPI::getCommandBuffer() const
{
    return (__bridge void*)impl->commandBuffer;
}

void* MetalRenderAPI::getRenderPassDescriptor() const
{
    return (__bridge void*)impl->imguiRenderPassDesc;
}

void* MetalRenderAPI::getRenderCommandEncoder() const
{
    return (__bridge void*)impl->encoder;
}

// ============================================================================
// Autorelease Pool
// ============================================================================

void MetalRenderAPI::executeWithAutoreleasePool(std::function<void()> fn)
{
    @autoreleasepool {
        fn();
    }
}

// ============================================================================
// C wrapper functions for ImGui Metal backend (called from ImGuiManager.cpp)
// These allow the C++ ImGuiManager to use the Objective-C ImGui Metal backend.
// ============================================================================

extern "C" bool ImGuiMetal_Init(void* device)
{
    id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
    return ImGui_ImplMetal_Init(mtlDevice);
}

extern "C" void ImGuiMetal_Shutdown()
{
    ImGui_ImplMetal_Shutdown();
}

extern "C" void ImGuiMetal_NewFrame(void* renderPassDescriptor)
{
    MTLRenderPassDescriptor* desc = (__bridge MTLRenderPassDescriptor*)renderPassDescriptor;
    if (desc) {
        ImGui_ImplMetal_NewFrame(desc);
    }
}
