#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif

#include "MetalRenderAPI.hpp"
#include <glm/ext/matrix_clip_space.hpp>
#include "MetalMesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include "Utils/Vertex.hpp"
#include "Utils/EnginePaths.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"

#include "stb_image.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stack>
#include <unordered_map>
#include <array>

// ============================================================================
// UBO structures matching Metal shaders
// ============================================================================

struct MetalGlobalUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrices[4];
    glm::vec4 cascadeSplits;
    glm::vec3 lightDir; float cascadeSplit4;
    glm::vec3 lightAmbient; int cascadeCount;
    glm::vec3 lightDiffuse; int debugCascades;
    glm::vec3 color; int useTexture;
};

struct MetalShadowUBO {
    glm::mat4 lightSpaceMatrix;
};

struct MetalSkyboxUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 sunDirection;
    float time;
};

struct MetalFXAAUniforms {
    glm::vec2 inverseScreenSize;
};

// ============================================================================
// Fullscreen quad vertices for FXAA
// ============================================================================
struct FXAAVertex {
    float x, y;
    float u, v;
};

static const FXAAVertex fxaaQuadVertices[] = {
    {-1.0f, -1.0f,  0.0f, 1.0f},
    { 1.0f, -1.0f,  1.0f, 1.0f},
    { 1.0f,  1.0f,  1.0f, 0.0f},
    {-1.0f, -1.0f,  0.0f, 1.0f},
    { 1.0f,  1.0f,  1.0f, 0.0f},
    {-1.0f,  1.0f,  0.0f, 0.0f},
};

// ============================================================================
// Skybox cube vertices
// ============================================================================
static const float skyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f
};

// ============================================================================
// Metal texture wrapper
// ============================================================================
struct MetalTexture {
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
};

// ============================================================================
// Implementation struct (Pimpl)
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

    void createPIEViewportTextures(PIEViewportTarget& target, int w, int h)
    {
        target.colorTexture = nil;
        target.depthTexture = nil;
        target.offscreenTexture = nil;
        target.offscreenDepthTexture = nil;
        target.width = w;
        target.height = h;

        // Final output color texture (what ImGui will sample)
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        target.colorTexture = [device newTextureWithDescriptor:desc];

        // Depth texture
        target.depthTexture = createDepthTextureWithSize(w, h);

        // Offscreen texture for FXAA intermediate rendering
        target.offscreenTexture = [device newTextureWithDescriptor:desc];
        target.offscreenDepthTexture = createDepthTextureWithSize(w, h);
    }

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
    // Helper methods
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

    bool loadShaderLibrary()
    {
        NSError* error = nil;

        // Try loading precompiled metallib
        NSString* libPath = [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/shaders.metallib").c_str()];
        NSURL* libURL = [NSURL fileURLWithPath:libPath];

        if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
            shaderLibrary = [device newLibraryWithURL:libURL error:&error];
            if (shaderLibrary) {
                printf("[Metal] Loaded precompiled shader library\n");
                return true;
            }
            printf("[Metal] Failed to load metallib: %s\n", [[error localizedDescription] UTF8String]);
        }

        // Fallback: compile from source
        // Read all .metal files and concatenate
        NSArray<NSString*>* shaderFiles = @[
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/basic_vertex.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/basic_fragment.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/shadow_vertex.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/shadow_fragment.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/sky_vertex.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/sky_fragment.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/fxaa_vertex.metal").c_str()],
            [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/fxaa_fragment.metal").c_str()]];

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
                printf("[Metal] Warning: Could not read shader file: %s\n", [path UTF8String]);
            }
        }

        if ([allSource length] == 0) {
            printf("[Metal] No shader sources found\n");
            return false;
        }

        MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
        shaderLibrary = [device newLibraryWithSource:allSource options:opts error:&error];
        if (!shaderLibrary) {
            printf("[Metal] Shader compilation failed: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }

        printf("[Metal] Compiled shaders from source\n");
        return true;
    }

    bool createPipelines()
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

    void createDepthStencilStates()
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

    void createShadowResources()
    {
        if (shadowQuality == 0) return;

        // Shadow map array texture (4 cascades)
        MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType = MTLTextureType2DArray;
        desc.pixelFormat = MTLPixelFormatDepth32Float;
        desc.width = shadowMapSize;
        desc.height = shadowMapSize;
        desc.arrayLength = NUM_CASCADES;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        shadowMapArray = [device newTextureWithDescriptor:desc];

        // Clear shadow map to max depth (1.0) so unrendered cascades don't cause artifacts
        id<MTLCommandBuffer> clearCmd = [commandQueue commandBuffer];
        for (int i = 0; i < NUM_CASCADES; i++) {
            MTLRenderPassDescriptor* clearPass = [MTLRenderPassDescriptor renderPassDescriptor];
            clearPass.depthAttachment.texture = shadowMapArray;
            clearPass.depthAttachment.slice = i;
            clearPass.depthAttachment.loadAction = MTLLoadActionClear;
            clearPass.depthAttachment.storeAction = MTLStoreActionStore;
            clearPass.depthAttachment.clearDepth = 1.0;
            id<MTLRenderCommandEncoder> enc = [clearCmd renderCommandEncoderWithDescriptor:clearPass];
            [enc endEncoding];
        }
        [clearCmd commit];
        [clearCmd waitUntilCompleted];

        // Shadow sampler with comparison for hardware-accelerated PCF
        MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
        sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.compareFunction = MTLCompareFunctionLessEqual;
        shadowSampler = [device newSamplerStateWithDescriptor:sampDesc];
    }

    void createOffscreenResources(int w, int h)
    {
        if (w <= 0 || h <= 0) return;

        // Offscreen color texture for FXAA input
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        offscreenTexture = [device newTextureWithDescriptor:desc];

        // Offscreen depth texture
        offscreenDepthTexture = createDepthTextureWithSize(w, h);
    }

    void createOffscreenResources()
    {
        createOffscreenResources(viewportWidth, viewportHeight);
    }

    void createViewportResources(int w, int h)
    {
        viewportTexture = nil;
        viewportDepthTexture = nil;
        viewportWidthRT = w;
        viewportHeightRT = h;

        // Viewport color texture (render target + shader read for ImGui display)
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        viewportTexture = [device newTextureWithDescriptor:desc];

        // Viewport depth texture
        viewportDepthTexture = createDepthTextureWithSize(w, h);
    }

    void createDefaultTexture()
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

    // CSM helpers
    void calculateCascadeSplits(float nearPlane, float farPlane)
    {
        cascadeSplitDistances[0] = nearPlane;
        for (int i = 1; i <= NUM_CASCADES; i++) {
            float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
            float log_split = nearPlane * std::pow(farPlane / nearPlane, p);
            float linear = nearPlane + (farPlane - nearPlane) * p;
            cascadeSplitDistances[i] = cascadeSplitLambda * log_split + (1.0f - cascadeSplitLambda) * linear;
        }
    }

    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
    {
        const glm::mat4 inv = glm::inverse(proj * view);
        std::array<glm::vec3, 8> corners;
        int idx = 0;
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                for (int z = 0; z < 2; ++z) {
                    glm::vec4 pt = inv * glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f, 2.0f * z - 1.0f, 1.0f);
                    corners[idx++] = glm::vec3(pt) / pt.w;
                }
            }
        }
        return corners;
    }

    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                             const glm::mat4& viewMat, float fov, float aspect)
    {
        float cascadeNear = cascadeSplitDistances[cascadeIndex];
        float cascadeFar = cascadeSplitDistances[cascadeIndex + 1];

        glm::mat4 cascadeProj = glm::perspectiveRH_ZO(glm::radians(fov), aspect, cascadeNear, cascadeFar);
        auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMat);

        glm::vec3 center(0.0f);
        for (const auto& c : corners) center += c;
        center /= 8.0f;

        glm::vec3 direction = glm::normalize(lightDir);
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(direction, up)) > 0.99f)
            up = glm::vec3(0.0f, 0.0f, 1.0f);

        glm::mat4 lightView = glm::lookAt(center - direction * 100.0f, center, up);

        float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max(), maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max(), maxZ = std::numeric_limits<float>::lowest();

        for (const auto& c : corners) {
            glm::vec4 lc = lightView * glm::vec4(c, 1.0f);
            minX = std::min(minX, lc.x); maxX = std::max(maxX, lc.x);
            minY = std::min(minY, lc.y); maxY = std::max(maxY, lc.y);
            minZ = std::min(minZ, lc.z); maxZ = std::max(maxZ, lc.z);
        }

        float padding = 10.0f;
        minZ -= padding;
        maxZ += 500.0f;

        return glm::orthoRH_ZO(minX, maxX, minY, maxY, minZ, maxZ) * lightView;
    }

    void recreateShadowResources(uint32_t size)
    {
        shadowMapSize = size;
        shadowMapArray = nil;
        shadowSampler = nil;
        createShadowResources();
    }
};

// ============================================================================
// MetalRenderAPI implementation
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
        printf("[Metal] Failed to create Metal device\n");
        return false;
    }
    printf("[Metal] Using device: %s\n", [[impl->device name] UTF8String]);
    printf("[Metal] GPU Family: Apple %s\n",
           [impl->device supportsFamily:MTLGPUFamilyApple7] ? "7+" :
           [impl->device supportsFamily:MTLGPUFamilyApple6] ? "6+" :
           [impl->device supportsFamily:MTLGPUFamilyApple5] ? "5+" : "< 5");
    printf("[Metal] Max buffer length: %lu MB\n", (unsigned long)([impl->device maxBufferLength] / (1024 * 1024)));
    printf("[Metal] Recommended max working set: %lu MB\n", (unsigned long)([impl->device recommendedMaxWorkingSetSize] / (1024 * 1024)));

    // Create command queue
    impl->commandQueue = [impl->device newCommandQueue];
    if (!impl->commandQueue) {
        printf("[Metal] Failed to create command queue\n");
        return false;
    }

    // Create frame semaphore
    impl->frameSemaphore = dispatch_semaphore_create(MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT);

    // Set up CAMetalLayer on the SDL window
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        printf("[Metal] Failed to get window info: %s\n", SDL_GetError());
        return false;
    }

    NSWindow* nsWindow = wmInfo.info.cocoa.window;
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
        printf("[Metal] Failed to create depth texture\n");
        return false;
    }

    // Load shaders
    if (!impl->loadShaderLibrary()) {
        printf("[Metal] Failed to load shader library\n");
        return false;
    }

    // Create pipeline states
    if (!impl->createPipelines()) {
        printf("[Metal] Failed to create pipelines\n");
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

    printf("[Metal] Shadow map: %ux%u (%d cascades, quality=%d)\n",
           impl->shadowMapSize, impl->shadowMapSize, MetalRenderAPIImpl::NUM_CASCADES, impl->shadowQuality);
    printf("[Metal] FXAA: %s\n", impl->fxaaInitialized ? "enabled" : "disabled");
    printf("[Metal] Render API initialized (%dx%d, FOV: %.1f)\n", width, height, fov);
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

void MetalRenderAPI::resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    impl->viewportWidth = width;
    impl->viewportHeight = height;

    if (impl->metalLayer) {
        impl->metalLayer.drawableSize = CGSizeMake(width, height);
    }

    impl->depthTexture = impl->createDepthTextureWithSize(width, height);
    impl->createOffscreenResources();

    float ratio = (float)width / (float)height;
    impl->projectionMatrix = glm::perspectiveRH_ZO(glm::radians(impl->fieldOfView), ratio, 0.1f, 1000.0f);
}

void MetalRenderAPI::beginFrame()
{
    // Ensure command buffer exists (may already be created by shadow pass)
    if (!impl->ensureCommandBuffer()) return;

    bool editorMode = (impl->viewportTexture != nil);

    // In editor mode, defer drawable acquisition to renderUI
    if (!editorMode) {
        if (!impl->ensureDrawable()) return;
    }

    // If shadow pass already created the main render encoder (via endShadowPass), skip
    if (impl->mainPassActive && impl->encoder) {
        // In game mode, update ImGui with the current render pass info
        if (!editorMode) {
            MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
            if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenTexture) {
                desc.colorAttachments[0].texture = impl->offscreenTexture;
            } else {
                desc.colorAttachments[0].texture = impl->currentDrawable.texture;
            }
            ImGui_ImplMetal_NewFrame(desc);
        }
        // In editor mode, ImGui_ImplMetal_NewFrame is deferred to renderUI
        return;
    }

    // Check if rendering to a PIE viewport
    bool pieMode = false;
    MetalRenderAPIImpl::PIEViewportTarget* pieTarget = nullptr;
    if (impl->activeSceneTarget >= 0) {
        auto it = impl->pieViewports.find(impl->activeSceneTarget);
        if (it != impl->pieViewports.end() && it->second.colorTexture) {
            pieMode = true;
            pieTarget = &it->second;
        }
    }

    // Determine render target dimensions
    int rtWidth, rtHeight;
    if (pieMode) {
        rtWidth = pieTarget->width;
        rtHeight = pieTarget->height;
    } else if (editorMode) {
        rtWidth = impl->viewportWidthRT;
        rtHeight = impl->viewportHeightRT;
    } else {
        rtWidth = impl->viewportWidth;
        rtHeight = impl->viewportHeight;
    }

    // Start the main render pass
    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];

    if (pieMode) {
        // PIE viewport: render to offscreen texture (for FXAA) or directly to color texture
        if (impl->fxaaEnabled && impl->fxaaInitialized && pieTarget->offscreenTexture) {
            passDesc.colorAttachments[0].texture = pieTarget->offscreenTexture;
        } else {
            passDesc.colorAttachments[0].texture = pieTarget->colorTexture;
        }
    } else if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenTexture) {
        // Render to offscreen texture for FXAA
        passDesc.colorAttachments[0].texture = impl->offscreenTexture;
    } else if (editorMode) {
        // Render directly to viewport texture (no FXAA)
        passDesc.colorAttachments[0].texture = impl->viewportTexture;
    } else {
        passDesc.colorAttachments[0].texture = impl->currentDrawable.texture;
    }
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(impl->clearColor.r, impl->clearColor.g, impl->clearColor.b, 1.0);

    // Depth texture
    id<MTLTexture> depthTex;
    if (pieMode) {
        if (impl->fxaaEnabled && impl->fxaaInitialized && pieTarget->offscreenDepthTexture) {
            depthTex = pieTarget->offscreenDepthTexture;
        } else {
            depthTex = pieTarget->depthTexture;
        }
    } else if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenDepthTexture) {
        depthTex = impl->offscreenDepthTexture;
    } else if (editorMode) {
        depthTex = impl->viewportDepthTexture;
    } else {
        depthTex = impl->depthTexture;
    }
    passDesc.depthAttachment.texture = depthTex;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.storeAction = MTLStoreActionStore;
    passDesc.depthAttachment.clearDepth = 1.0;

    // Update ImGui Metal backend (game mode only; editor defers to renderUI)
    if (!editorMode) {
        ImGui_ImplMetal_NewFrame(passDesc);
    }

    impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    if (!impl->encoder) {
        printf("[Metal] beginFrame: Failed to create render encoder\n");
        return;
    }
    impl->encoder.label = @"Main Render Encoder";
    impl->mainPassActive = true;

    // Set viewport to render target dimensions
    MTLViewport viewport = {0, 0, (double)rtWidth, (double)rtHeight, 0, 1};
    [impl->encoder setViewport:viewport];

    // Set default depth stencil state
    [impl->encoder setDepthStencilState:impl->depthLessEqual];

    // Set default cull mode
    [impl->encoder setCullMode:MTLCullModeBack];
    [impl->encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    // Reset model matrix
    impl->currentModelMatrix = glm::mat4(1.0f);
    while (!impl->modelMatrixStack.empty()) impl->modelMatrixStack.pop();

    // Reset bind tracking
    impl->lastBoundPipeline = nil;
    impl->lastBoundDepthStencil = nil;
    impl->lastBoundVertexBuffer = nil;
    impl->lastBoundTextureHandle = INVALID_TEXTURE;
    impl->lastCullMode = MTLCullModeBack;
    impl->shadowMapBound = false;
    impl->perFrameUBOReady = false;
    impl->drawCallCount = 0;

    impl->frameStarted = true;
}

void MetalRenderAPI::endFrame()
{
    if (!impl->frameStarted) return;

    // In editor mode, endSceneRender + renderUI handle finalization
    if (impl->viewportTexture) return;

    // End main render encoder
    if (impl->mainPassActive && impl->encoder) {
        // If FXAA is disabled, render ImGui here
        if (!impl->fxaaEnabled || !impl->fxaaInitialized)
        {
            ImDrawData* drawData = ImGui::GetDrawData();
            if (drawData && drawData->TotalVtxCount > 0) {
                ImGui_ImplMetal_RenderDrawData(drawData, impl->commandBuffer, impl->encoder);
            }
        }

        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }

    // Apply FXAA if enabled
    if (impl->fxaaEnabled && impl->fxaaInitialized && impl->fxaaPipeline && impl->offscreenTexture)
    {
        MTLRenderPassDescriptor* fxaaPass = [MTLRenderPassDescriptor renderPassDescriptor];
        fxaaPass.colorAttachments[0].texture = impl->currentDrawable.texture;
        fxaaPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        fxaaPass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> fxaaEncoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:fxaaPass];
        if (!fxaaEncoder) {
            printf("[Metal] endFrame: Failed to create FXAA encoder\n");
        } else {
            fxaaEncoder.label = @"FXAA Render Encoder";

            [fxaaEncoder setRenderPipelineState:impl->fxaaPipeline];

            MTLViewport viewport = {0, 0, (double)impl->viewportWidth, (double)impl->viewportHeight, 0, 1};
            [fxaaEncoder setViewport:viewport];

            // Bind offscreen texture as input
            [fxaaEncoder setFragmentTexture:impl->offscreenTexture atIndex:0];
            [fxaaEncoder setFragmentSamplerState:impl->defaultSampler atIndex:0];

            // Push inverse screen size
            MetalFXAAUniforms fxaaUniforms;
            fxaaUniforms.inverseScreenSize = glm::vec2(1.0f / impl->viewportWidth, 1.0f / impl->viewportHeight);
            [fxaaEncoder setFragmentBytes:&fxaaUniforms length:sizeof(fxaaUniforms) atIndex:0];

            // Draw fullscreen quad
            [fxaaEncoder setVertexBuffer:impl->fxaaVertexBuffer offset:0 atIndex:0];
            [fxaaEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

            // Render ImGui overlay after FXAA
            ImDrawData* drawData = ImGui::GetDrawData();
            if (drawData && drawData->TotalVtxCount > 0) {
                ImGui_ImplMetal_RenderDrawData(drawData, impl->commandBuffer, fxaaEncoder);
            }

            [fxaaEncoder endEncoding];
        }
    }

    // Present and commit
    if (impl->currentDrawable) {
        [impl->commandBuffer presentDrawable:impl->currentDrawable];
    }

#ifdef _DEBUG
    // Log first few frames for diagnostics
    if (impl->frameNumber < 3) {
        printf("[Metal] Frame %u: %u draw calls\n", impl->frameNumber, impl->drawCallCount);
        fflush(stdout);
    }
#endif

    // Signal semaphore on completion and log GPU errors with enhanced info
    __block dispatch_semaphore_t sem = impl->frameSemaphore;
    __block uint32_t frameNum = impl->frameNumber;
    __block uint32_t* errorCountPtr = &impl->gpuErrorCount;
    __block id<MTLDevice> dev = impl->device;
    __block id<MTLCommandQueue> __strong* queuePtr = &impl->commandQueue;
    [impl->commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buf) {
        if (buf.status == MTLCommandBufferStatusError) {
            (*errorCountPtr)++;
            printf("[Metal] GPU Error (frame %u, total errors: %u): %s\n",
                   frameNum, *errorCountPtr, [[buf.error localizedDescription] UTF8String]);

            // Log per-encoder execution status
            if (@available(macOS 11.0, *)) {
                NSArray<id<MTLCommandBufferEncoderInfo>>* encoderInfos = buf.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
                for (id<MTLCommandBufferEncoderInfo> info in encoderInfos) {
                    NSString* statusStr = @"unknown";
                    switch (info.errorState) {
                        case MTLCommandEncoderErrorStateCompleted: statusStr = @"completed"; break;
                        case MTLCommandEncoderErrorStateAffected: statusStr = @"affected"; break;
                        case MTLCommandEncoderErrorStateFaulted: statusStr = @"FAULTED"; break;
                        case MTLCommandEncoderErrorStatePending: statusStr = @"pending"; break;
                        default: break;
                    }
                    printf("[Metal]   Encoder '%s': %s\n", [info.label UTF8String], [statusStr UTF8String]);
                }
            }
            fflush(stdout);

            // Auto-recover: recreate command queue after persistent errors
            if (*errorCountPtr >= MetalRenderAPIImpl::MAX_GPU_ERRORS_BEFORE_RECOVERY) {
                printf("[Metal] Too many consecutive GPU errors, recreating command queue\n");
                *queuePtr = [dev newCommandQueue];
                *errorCountPtr = 0;
            }
        } else {
            // Reset error counter on successful frame
            *errorCountPtr = 0;
        }
        dispatch_semaphore_signal(sem);
    }];

    [impl->commandBuffer commit];

    impl->commandBuffer = nil;
    impl->currentDrawable = nil;
    impl->frameStarted = false;
    impl->frameNumber++;
    impl->currentFrame = (impl->currentFrame + 1) % MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT;
}

void MetalRenderAPI::present()
{
    // Presentation happens in endFrame via presentDrawable
}

void MetalRenderAPI::clear(const glm::vec3& color)
{
    impl->clearColor = color;
}

void MetalRenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    impl->viewMatrix = glm::lookAt(pos, target, up);
}

void MetalRenderAPI::pushMatrix()
{
    impl->modelMatrixStack.push(impl->currentModelMatrix);
}

void MetalRenderAPI::popMatrix()
{
    if (!impl->modelMatrixStack.empty()) {
        impl->currentModelMatrix = impl->modelMatrixStack.top();
        impl->modelMatrixStack.pop();
    }
}

void MetalRenderAPI::translate(const glm::vec3& pos)
{
    impl->currentModelMatrix = glm::translate(impl->currentModelMatrix, pos);
}

void MetalRenderAPI::rotate(const glm::mat4& rotation)
{
    impl->currentModelMatrix = impl->currentModelMatrix * rotation;
}

void MetalRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    impl->currentModelMatrix = impl->currentModelMatrix * matrix;
}

glm::mat4 MetalRenderAPI::getProjectionMatrix() const
{
    return impl->projectionMatrix;
}

glm::mat4 MetalRenderAPI::getViewMatrix() const
{
    return impl->viewMatrix;
}

// ============================================================================
// Texture Management
// ============================================================================

TextureHandle MetalRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(invert_y);
    unsigned char* pixels = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    if (!pixels) {
        LOG_ENGINE_ERROR("[Metal] Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    TextureHandle handle = loadTextureFromMemory(pixels, width, height, 4, false, generate_mipmaps);
    stbi_image_free(pixels);
    return handle;
}

TextureHandle MetalRenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                      bool flip_vertically, bool generate_mipmaps)
{
    if (!pixels || width <= 0 || height <= 0) return INVALID_TEXTURE;

    // Convert to RGBA if needed
    std::vector<uint8_t> rgbaData;
    const uint8_t* srcData = pixels;
    if (channels != 4) {
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++) {
            rgbaData[i * 4 + 0] = pixels[i * channels + 0];
            rgbaData[i * 4 + 1] = channels > 1 ? pixels[i * channels + 1] : pixels[i * channels];
            rgbaData[i * 4 + 2] = channels > 2 ? pixels[i * channels + 2] : pixels[i * channels];
            rgbaData[i * 4 + 3] = channels > 3 ? pixels[i * channels + 3] : 255;
        }
        srcData = rgbaData.data();
    }

    // Handle vertical flip
    std::vector<uint8_t> flippedData;
    if (flip_vertically) {
        flippedData.resize(width * height * 4);
        size_t row_size = width * 4;
        for (int y = 0; y < height; ++y) {
            memcpy(flippedData.data() + y * row_size, srcData + (height - 1 - y) * row_size, row_size);
        }
        srcData = flippedData.data();
    }

    uint32_t mipLevels = generate_mipmaps ?
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:width
                                                                                  height:height
                                                                               mipmapped:(mipLevels > 1)];
    desc.usage = MTLTextureUsageShaderRead;
    if (mipLevels > 1) {
        desc.mipmapLevelCount = mipLevels;
    }

    id<MTLTexture> texture = [impl->device newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:srcData bytesPerRow:width * 4];

    // Generate mipmaps
    if (mipLevels > 1) {
        id<MTLCommandBuffer> blitCmd = [impl->commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> blitEncoder = [blitCmd blitCommandEncoder];
        [blitEncoder generateMipmapsForTexture:texture];
        [blitEncoder endEncoding];
        [blitCmd commit];
        [blitCmd waitUntilCompleted];
    }

    // Create sampler
    MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
    sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.mipFilter = (mipLevels > 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    sampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.maxAnisotropy = 16;
    id<MTLSamplerState> sampler = [impl->device newSamplerStateWithDescriptor:sampDesc];

    MetalTexture metalTex;
    metalTex.texture = texture;
    metalTex.sampler = sampler;
    metalTex.width = width;
    metalTex.height = height;
    metalTex.mipLevels = mipLevels;

    TextureHandle handle = impl->nextTextureHandle++;
    impl->textures[handle] = metalTex;

    LOG_ENGINE_TRACE("[Metal] Loaded texture -> handle {} ({}x{}, mips={})", handle, width, height, mipLevels);
    return handle;
}

TextureHandle MetalRenderAPI::loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                     const std::vector<const uint8_t*>& mip_data,
                                                     const std::vector<size_t>& mip_sizes,
                                                     const std::vector<std::pair<int,int>>& mip_dimensions)
{
    if (mip_count <= 0 || mip_data.empty()) return INVALID_TEXTURE;

    MTLPixelFormat mtlFormat;
    NSUInteger blockSize = 0;
    bool isBC = false;
    switch (format) {
    case 0: mtlFormat = MTLPixelFormatRGBA8Unorm; break;
    case 1: mtlFormat = MTLPixelFormatBC1_RGBA; blockSize = 8; isBC = true; break;
    case 2: mtlFormat = MTLPixelFormatBC3_RGBA; blockSize = 16; isBC = true; break;
    case 3: mtlFormat = MTLPixelFormatBC5_RGUnorm; blockSize = 16; isBC = true; break;
    case 4: mtlFormat = MTLPixelFormatBC7_RGBAUnorm; blockSize = 16; isBC = true; break;
    default: return INVALID_TEXTURE;
    }

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtlFormat
                                                                                   width:width
                                                                                  height:height
                                                                               mipmapped:(mip_count > 1)];
    desc.usage = MTLTextureUsageShaderRead;
    desc.mipmapLevelCount = mip_count;
    desc.storageMode = MTLStorageModeManaged;

    id<MTLTexture> texture = [impl->device newTextureWithDescriptor:desc];
    if (!texture) return INVALID_TEXTURE;

    for (int i = 0; i < mip_count; ++i) {
        int mw = mip_dimensions[i].first;
        int mh = mip_dimensions[i].second;
        NSUInteger bytesPerRow;

        if (isBC) {
            NSUInteger blockWidth = (mw + 3) / 4;
            bytesPerRow = blockWidth * blockSize;
        } else {
            bytesPerRow = mw * 4;
        }

        [texture replaceRegion:MTLRegionMake2D(0, 0, mw, mh)
                   mipmapLevel:i
                     withBytes:mip_data[i]
                   bytesPerRow:bytesPerRow];
    }

    MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
    sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.mipFilter = (mip_count > 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    sampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
    sampDesc.maxAnisotropy = 16;
    id<MTLSamplerState> sampler = [impl->device newSamplerStateWithDescriptor:sampDesc];

    MetalTexture metalTex;
    metalTex.texture = texture;
    metalTex.sampler = sampler;
    metalTex.width = width;
    metalTex.height = height;
    metalTex.mipLevels = mip_count;

    TextureHandle handle = impl->nextTextureHandle++;
    impl->textures[handle] = metalTex;
    LOG_ENGINE_TRACE("[Metal] loadCompressedTexture: handle {} ({}x{}, {} mips, format {})", handle, width, height, mip_count, format);
    return handle;
}

void MetalRenderAPI::bindTexture(TextureHandle texture)
{
    impl->boundTexture = texture;
}

void MetalRenderAPI::unbindTexture()
{
    impl->boundTexture = INVALID_TEXTURE;
}

void MetalRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE) {
        impl->textures.erase(texture);
    }
}

// ============================================================================
// Mesh Rendering
// ============================================================================

void MetalRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!impl->frameStarted || !m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<MetalRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(m.gpu_mesh);
    if (!metalMesh) return;
    id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
    if (!vertexBuffer) return;

    if (impl->inShadowPass) {
        // Shadow pass rendering
        MetalShadowUBO shadowUBO;
        shadowUBO.lightSpaceMatrix = impl->lightSpaceMatrices[impl->currentCascade];
        [impl->encoder setVertexBytes:&shadowUBO length:sizeof(shadowUBO) atIndex:1];
        [impl->encoder setVertexBytes:&impl->currentModelMatrix length:sizeof(glm::mat4) atIndex:2];
        [impl->encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        if (metalMesh->isIndexed()) {
            id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
            [impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                      indexCount:metalMesh->getIndexCount()
                                       indexType:MTLIndexTypeUInt32
                                     indexBuffer:indexBuffer
                               indexBufferOffset:0];
        } else {
            [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:metalMesh->getVertexCount()];
        }
        return;
    }

    // Main pass rendering
    // Select pipeline based on blend mode
    id<MTLRenderPipelineState> pipeline = impl->basicPipeline;
    switch (state.blend_mode) {
        case BlendMode::Alpha:    pipeline = impl->basicPipelineAlpha; break;
        case BlendMode::Additive: pipeline = impl->basicPipelineAdditive; break;
        default: break;
    }
    [impl->encoder setRenderPipelineState:pipeline];

    // Set depth state
    switch (state.depth_test) {
        case DepthTest::LessEqual:
            [impl->encoder setDepthStencilState:state.depth_write ? impl->depthLessEqual : impl->depthLessEqualNoWrite];
            break;
        case DepthTest::Less:
            [impl->encoder setDepthStencilState:state.depth_write ? impl->depthLessEqual : impl->depthLessEqualNoWrite];
            break;
        case DepthTest::None:
            [impl->encoder setDepthStencilState:impl->depthNone];
            break;
    }

    // Set cull mode
    switch (state.cull_mode) {
        case CullMode::Back:  [impl->encoder setCullMode:MTLCullModeBack]; break;
        case CullMode::Front: [impl->encoder setCullMode:MTLCullModeFront]; break;
        case CullMode::None:  [impl->encoder setCullMode:MTLCullModeNone]; break;
    }

    // Build UBO from cached per-frame data, update per-draw fields
    impl->updatePerFrameUBO();
    MetalGlobalUBO ubo = impl->cachedPerFrameUBO;
    ubo.color = state.color;
    ubo.useTexture = (m.texture_set && m.texture != INVALID_TEXTURE) ? 1 : 0;

    [impl->encoder setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
    [impl->encoder setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [impl->encoder setFragmentBytes:&impl->currentLights length:sizeof(LightCBuffer) atIndex:3];

    // Set model matrix
    [impl->encoder setVertexBytes:&impl->currentModelMatrix length:sizeof(glm::mat4) atIndex:2];

    // Bind texture
    TextureHandle texHandle = (m.texture_set && m.texture != INVALID_TEXTURE) ? m.texture : INVALID_TEXTURE;
    if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
        auto& tex = impl->textures[texHandle];
        [impl->encoder setFragmentTexture:tex.texture atIndex:0];
        [impl->encoder setFragmentSamplerState:tex.sampler atIndex:0];
    } else {
        [impl->encoder setFragmentTexture:impl->defaultTexture atIndex:0];
        [impl->encoder setFragmentSamplerState:impl->defaultSampler atIndex:0];
    }

    if (impl->shadowMapArray && !impl->shadowMapBound) {
        [impl->encoder setFragmentTexture:impl->shadowMapArray atIndex:1];
        [impl->encoder setFragmentSamplerState:impl->shadowSampler atIndex:1];
        impl->shadowMapBound = true;
    }

    // Draw
    [impl->encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    if (metalMesh->isIndexed()) {
        id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
        size_t idxCount = metalMesh->getIndexCount();
        size_t maxIndex = [indexBuffer length] / sizeof(uint32_t);
        if (idxCount > maxIndex) {
            printf("[Metal] WARNING: index count %zu exceeds index buffer capacity %zu\n", idxCount, maxIndex);
            idxCount = maxIndex;
        }
        [impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                  indexCount:idxCount
                                   indexType:MTLIndexTypeUInt32
                                 indexBuffer:indexBuffer
                           indexBufferOffset:0];
    } else {
        size_t bufLen = [vertexBuffer length];
        size_t drawCount = metalMesh->getVertexCount();
        size_t maxVertex = bufLen / sizeof(vertex);
        if (drawCount > maxVertex) {
            printf("[Metal] WARNING: draw count %zu exceeds buffer capacity %zu (buf=%p, len=%zu)\n",
                   drawCount, maxVertex, (__bridge void*)vertexBuffer, bufLen);
            drawCount = maxVertex;
        }
        [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:drawCount];
    }
#ifdef _DEBUG
    static bool firstDraw = true;
    if (firstDraw) {
        printf("[Metal] First draw: vertices=%zu, indexed=%d, buffer=%p\n",
               metalMesh->getVertexCount(), metalMesh->isIndexed(), (__bridge void*)vertexBuffer);
        firstDraw = false;
    }
#endif
    impl->drawCallCount++;
}

void MetalRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!impl->frameStarted || !m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0) return;

    if (start_vertex + vertex_count > m.vertices_len) {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<MetalRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(m.gpu_mesh);
    if (!metalMesh) return;
    id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
    if (!vertexBuffer) return;

    if (impl->inShadowPass) {
        MetalShadowUBO shadowUBO;
        shadowUBO.lightSpaceMatrix = impl->lightSpaceMatrices[impl->currentCascade];
        [impl->encoder setVertexBytes:&shadowUBO length:sizeof(shadowUBO) atIndex:1];
        [impl->encoder setVertexBytes:&impl->currentModelMatrix length:sizeof(glm::mat4) atIndex:2];
        if (vertexBuffer != impl->lastBoundVertexBuffer) {
            [impl->encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
            impl->lastBoundVertexBuffer = vertexBuffer;
        }
        if (metalMesh->isIndexed()) {
            id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
            if (indexBuffer) {
                size_t maxIndex = [indexBuffer length] / sizeof(uint32_t);
                size_t endOffset = start_vertex + vertex_count;
                if (endOffset > maxIndex) {
                    vertex_count = maxIndex > start_vertex ? maxIndex - start_vertex : 0;
                }
                if (vertex_count > 0) {
                    [impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                              indexCount:vertex_count
                                               indexType:MTLIndexTypeUInt32
                                             indexBuffer:indexBuffer
                                       indexBufferOffset:start_vertex * sizeof(uint32_t)];
                }
            }
        } else {
            [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:start_vertex vertexCount:vertex_count];
        }
        impl->drawCallCount++;
        return;
    }

    // Main pass - optimized with redundant bind tracking
    id<MTLRenderPipelineState> pipeline = impl->basicPipeline;
    switch (state.blend_mode) {
        case BlendMode::Alpha:    pipeline = impl->basicPipelineAlpha; break;
        case BlendMode::Additive: pipeline = impl->basicPipelineAdditive; break;
        default: break;
    }
    if (pipeline != impl->lastBoundPipeline) {
        [impl->encoder setRenderPipelineState:pipeline];
        impl->lastBoundPipeline = pipeline;
    }

    id<MTLDepthStencilState> depthState = impl->depthLessEqual;
    switch (state.depth_test) {
        case DepthTest::LessEqual:
        case DepthTest::Less:
            depthState = state.depth_write ? impl->depthLessEqual : impl->depthLessEqualNoWrite;
            break;
        case DepthTest::None:
            depthState = impl->depthNone;
            break;
    }
    if (depthState != impl->lastBoundDepthStencil) {
        [impl->encoder setDepthStencilState:depthState];
        impl->lastBoundDepthStencil = depthState;
    }

    MTLCullMode cullMode = MTLCullModeBack;
    switch (state.cull_mode) {
        case CullMode::Back:  cullMode = MTLCullModeBack; break;
        case CullMode::Front: cullMode = MTLCullModeFront; break;
        case CullMode::None:  cullMode = MTLCullModeNone; break;
    }
    if (cullMode != impl->lastCullMode) {
        [impl->encoder setCullMode:cullMode];
        impl->lastCullMode = cullMode;
    }

    // Build UBO from cached per-frame data, update per-draw fields
    impl->updatePerFrameUBO();
    MetalGlobalUBO ubo = impl->cachedPerFrameUBO;
    ubo.color = state.color;
    ubo.useTexture = (impl->boundTexture != INVALID_TEXTURE) ? 1 : 0;

    [impl->encoder setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
    [impl->encoder setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [impl->encoder setFragmentBytes:&impl->currentLights length:sizeof(LightCBuffer) atIndex:3];

    // Model matrix per draw
    [impl->encoder setVertexBytes:&impl->currentModelMatrix length:sizeof(glm::mat4) atIndex:2];

    // Bind texture (with tracking)
    TextureHandle texHandle = impl->boundTexture;
    if (texHandle != impl->lastBoundTextureHandle) {
        if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
            auto& tex = impl->textures[texHandle];
            [impl->encoder setFragmentTexture:tex.texture atIndex:0];
            [impl->encoder setFragmentSamplerState:tex.sampler atIndex:0];
        } else {
            [impl->encoder setFragmentTexture:impl->defaultTexture atIndex:0];
            [impl->encoder setFragmentSamplerState:impl->defaultSampler atIndex:0];
        }
        impl->lastBoundTextureHandle = texHandle;
    }

    // Bind shadow map (once per encoder, tracked)
    if (impl->shadowMapArray && !impl->shadowMapBound) {
        [impl->encoder setFragmentTexture:impl->shadowMapArray atIndex:1];
        [impl->encoder setFragmentSamplerState:impl->shadowSampler atIndex:1];
        impl->shadowMapBound = true;
    }

    // Bind vertex buffer (with tracking)
    if (vertexBuffer != impl->lastBoundVertexBuffer) {
        [impl->encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        impl->lastBoundVertexBuffer = vertexBuffer;
    }

    if (metalMesh->isIndexed()) {
        id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
        if (indexBuffer) {
            size_t maxIndex = [indexBuffer length] / sizeof(uint32_t);
            size_t endOffset = start_vertex + vertex_count;
            if (endOffset > maxIndex) {
                vertex_count = maxIndex > start_vertex ? maxIndex - start_vertex : 0;
            }
            if (vertex_count > 0) {
                [impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                          indexCount:vertex_count
                                           indexType:MTLIndexTypeUInt32
                                         indexBuffer:indexBuffer
                                   indexBufferOffset:start_vertex * sizeof(uint32_t)];
            }
        }
    } else {
        [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:start_vertex vertexCount:vertex_count];
    }
    impl->drawCallCount++;
}

// ============================================================================
// State Management
// ============================================================================

void MetalRenderAPI::setRenderState(const RenderState& state)
{
    impl->currentState = state;
}

void MetalRenderAPI::enableLighting(bool enable)
{
    impl->lightingEnabled = enable;
}

void MetalRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    impl->lightAmbient = ambient;
    impl->lightDiffuse = diffuse;
    impl->lightDirection = glm::normalize(direction);
}

void MetalRenderAPI::setPointAndSpotLights(const LightCBuffer& lights)
{
    impl->currentLights = lights;
}

// ============================================================================
// Skybox
// ============================================================================

void MetalRenderAPI::renderSkybox()
{
    if (!impl->frameStarted || !impl->skyboxPipeline || impl->inShadowPass) return;
    if (!impl->mainPassActive || !impl->encoder) return;

    [impl->encoder setRenderPipelineState:impl->skyboxPipeline];
    [impl->encoder setDepthStencilState:impl->depthLessEqualNoWrite];
    [impl->encoder setCullMode:MTLCullModeNone];

    MetalSkyboxUBO skyUBO;
    skyUBO.view = impl->viewMatrix;
    skyUBO.projection = impl->projectionMatrix;
    skyUBO.sunDirection = -impl->lightDirection;
    skyUBO.time = 0.0f;

    [impl->encoder setVertexBytes:&skyUBO length:sizeof(skyUBO) atIndex:1];
    [impl->encoder setFragmentBytes:&skyUBO length:sizeof(skyUBO) atIndex:0];

    [impl->encoder setVertexBuffer:impl->skyboxVertexBuffer offset:0 atIndex:0];
    [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
}

// ============================================================================
// Shadow Mapping (CSM)
// ============================================================================

void MetalRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    if (impl->shadowQuality == 0 || !impl->shadowPipeline || !impl->shadowMapArray) {
        impl->inShadowPass = false;
        return;
    }

    // Ensure command buffer is ready (shadow pass runs before beginFrame)
    if (!impl->ensureCommandBuffer()) return;

    impl->inShadowPass = true;
    impl->frameStarted = true; // Mark as started so renderMesh works during shadow pass
    impl->calculateCascadeSplits(0.1f, 1000.0f);

    int rtWidth = impl->viewportTexture ? impl->viewportWidthRT : impl->viewportWidth;
    int rtHeight = impl->viewportTexture ? impl->viewportHeightRT : impl->viewportHeight;
    float aspect = static_cast<float>(rtWidth) / static_cast<float>(rtHeight);
    for (int i = 0; i < MetalRenderAPIImpl::NUM_CASCADES; i++) {
        impl->lightSpaceMatrices[i] = impl->getLightSpaceMatrixForCascade(i, lightDir, impl->viewMatrix, impl->fieldOfView, aspect);
    }
    impl->currentCascade = 0;
}

void MetalRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    if (impl->shadowQuality == 0 || !impl->shadowPipeline || !impl->shadowMapArray) {
        impl->inShadowPass = false;
        return;
    }

    // Ensure command buffer is ready (shadow pass runs before beginFrame)
    if (!impl->ensureCommandBuffer()) return;

    impl->inShadowPass = true;
    impl->frameStarted = true; // Mark as started so renderMesh works during shadow pass

    // Set view matrix from camera before calculating cascades
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    impl->viewMatrix = glm::lookAt(pos, target, up);

    impl->calculateCascadeSplits(0.1f, 1000.0f);

    int rtWidth = impl->viewportTexture ? impl->viewportWidthRT : impl->viewportWidth;
    int rtHeight = impl->viewportTexture ? impl->viewportHeightRT : impl->viewportHeight;
    float aspect = static_cast<float>(rtWidth) / static_cast<float>(rtHeight);
    for (int i = 0; i < MetalRenderAPIImpl::NUM_CASCADES; i++) {
        impl->lightSpaceMatrices[i] = impl->getLightSpaceMatrixForCascade(i, lightDir, impl->viewMatrix, impl->fieldOfView, aspect);
    }
    impl->currentCascade = 0;
}

void MetalRenderAPI::beginCascade(int cascadeIndex)
{
    if (!impl->inShadowPass) return;
    impl->currentCascade = cascadeIndex;

    // End current encoder if active
    if (impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }

    if (!impl->shadowMapArray || !impl->commandBuffer) {
        printf("[Metal] beginCascade(%d): shadowMapArray=%p, commandBuffer=%p - aborting\n",
               cascadeIndex, impl->shadowMapArray, impl->commandBuffer);
        impl->inShadowPass = false;
        return;
    }

    // Create shadow render pass for this cascade
    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    passDesc.depthAttachment.texture = impl->shadowMapArray;
    passDesc.depthAttachment.slice = cascadeIndex;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.storeAction = MTLStoreActionStore;
    passDesc.depthAttachment.clearDepth = 1.0;

    impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    if (!impl->encoder) {
        printf("[Metal] beginCascade(%d): Failed to create encoder\n", cascadeIndex);
        impl->inShadowPass = false;
        return;
    }
    impl->encoder.label = [NSString stringWithFormat:@"Shadow Cascade %d", cascadeIndex];

    [impl->encoder setRenderPipelineState:impl->shadowPipeline];
    [impl->encoder setDepthStencilState:impl->shadowDepthState];
    [impl->encoder setCullMode:MTLCullModeFront]; // Front-face culling reduces shadow acne

    MTLViewport viewport = {0, 0, (double)impl->shadowMapSize, (double)impl->shadowMapSize, 0, 1};
    [impl->encoder setViewport:viewport];

    // Reset bind tracking for shadow pass
    impl->lastBoundVertexBuffer = nil;
}

void MetalRenderAPI::endShadowPass()
{
    if (!impl->frameStarted || !impl->inShadowPass) return;

    // End shadow encoder
    if (impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
    }

    impl->inShadowPass = false;

    bool editorMode = (impl->viewportTexture != nil);

    // In editor mode, defer drawable acquisition to renderUI
    if (!editorMode) {
        if (!impl->ensureDrawable()) {
            printf("[Metal] endShadowPass: Failed to acquire drawable\n");
            return;
        }
    }

    // Determine render target dimensions
    int rtWidth = editorMode ? impl->viewportWidthRT : impl->viewportWidth;
    int rtHeight = editorMode ? impl->viewportHeightRT : impl->viewportHeight;

    // Restart main render pass
    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];

    if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenTexture) {
        passDesc.colorAttachments[0].texture = impl->offscreenTexture;
    } else if (editorMode) {
        passDesc.colorAttachments[0].texture = impl->viewportTexture;
    } else {
        passDesc.colorAttachments[0].texture = impl->currentDrawable.texture;
    }
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(impl->clearColor.r, impl->clearColor.g, impl->clearColor.b, 1.0);

    // Depth texture: editor mode uses viewport depth or offscreen depth
    id<MTLTexture> depthTex;
    if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenDepthTexture) {
        depthTex = impl->offscreenDepthTexture;
    } else if (editorMode) {
        depthTex = impl->viewportDepthTexture;
    } else {
        depthTex = impl->depthTexture;
    }
    passDesc.depthAttachment.texture = depthTex;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.storeAction = MTLStoreActionStore;
    passDesc.depthAttachment.clearDepth = 1.0;

    impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    if (!impl->encoder) {
        printf("[Metal] endShadowPass: Failed to create main encoder\n");
        impl->mainPassActive = false;
        return;
    }
    impl->encoder.label = @"Main Render Encoder (Post-Shadow)";
    impl->mainPassActive = true;

    // Restore viewport
    MTLViewport viewport = {0, 0, (double)rtWidth, (double)rtHeight, 0, 1};
    [impl->encoder setViewport:viewport];

    // Restore default state and reset bind tracking for fresh encoder
    [impl->encoder setDepthStencilState:impl->depthLessEqual];
    impl->lastBoundPipeline = nil;
    impl->lastBoundDepthStencil = nil;
    impl->lastBoundVertexBuffer = nil;
    impl->lastBoundTextureHandle = INVALID_TEXTURE;
    impl->lastCullMode = MTLCullModeBack;
    impl->shadowMapBound = false;
    impl->perFrameUBOReady = false;
    impl->drawCallCount = 0;
    [impl->encoder setCullMode:MTLCullModeBack];
    [impl->encoder setFrontFacingWinding:MTLWindingCounterClockwise];
}

void MetalRenderAPI::bindShadowMap(int textureUnit)
{
    // Shadow map is automatically bound during rendering
}

glm::mat4 MetalRenderAPI::getLightSpaceMatrix()
{
    return impl->lightSpaceMatrices[0];
}

int MetalRenderAPI::getCascadeCount() const
{
    return (impl->shadowQuality > 0 && impl->shadowPipeline) ? MetalRenderAPIImpl::NUM_CASCADES : 0;
}

const float* MetalRenderAPI::getCascadeSplitDistances() const
{
    return impl->cascadeSplitDistances;
}

const glm::mat4* MetalRenderAPI::getLightSpaceMatrices() const
{
    return impl->lightSpaceMatrices;
}

// ============================================================================
// Resource Creation
// ============================================================================

IGPUMesh* MetalRenderAPI::createMesh()
{
    MetalMesh* metalMesh = new MetalMesh();
    metalMesh->setDevice((__bridge void*)impl->device);
    metalMesh->setCommandQueue((__bridge void*)impl->commandQueue);
    return metalMesh;
}

// ============================================================================
// Graphics Settings
// ============================================================================

void MetalRenderAPI::setFXAAEnabled(bool enabled)
{
    impl->fxaaEnabled = enabled;
}

bool MetalRenderAPI::isFXAAEnabled() const
{
    return impl->fxaaEnabled;
}

void MetalRenderAPI::setShadowQuality(int quality)
{
    if (quality == impl->shadowQuality) return;

    impl->shadowQuality = quality;

    uint32_t sizes[] = {0, 1024, 2048, 4096};
    uint32_t newSize = (quality >= 0 && quality <= 3) ? sizes[quality] : 0;

    if (quality == 0) {
        impl->shadowMapArray = nil;
    } else {
        impl->recreateShadowResources(newSize);
    }
}

int MetalRenderAPI::getShadowQuality() const
{
    return impl->shadowQuality;
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
// Viewport rendering (for editor)
// ============================================================================

void MetalRenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == impl->viewportWidthRT && height == impl->viewportHeightRT) return;

    impl->createViewportResources(width, height);

    // Resize offscreen resources to match viewport panel dimensions (for FXAA)
    impl->offscreenTexture = nil;
    impl->offscreenDepthTexture = nil;
    impl->createOffscreenResources(width, height);

    // Update projection matrix to match viewport aspect ratio
    float ratio = (float)width / (float)height;
    impl->projectionMatrix = glm::perspectiveRH_ZO(glm::radians(impl->fieldOfView), ratio, 0.1f, 1000.0f);
}

void MetalRenderAPI::endSceneRender()
{
    // Check if we are rendering to a PIE viewport
    if (impl->activeSceneTarget >= 0)
    {
        auto it = impl->pieViewports.find(impl->activeSceneTarget);
        if (it != impl->pieViewports.end())
        {
            auto& pie = it->second;

            // End the main scene encoder
            if (impl->mainPassActive && impl->encoder) {
                [impl->encoder endEncoding];
                impl->encoder = nil;
                impl->mainPassActive = false;
            }

            // Apply FXAA from PIE offscreen -> PIE color texture
            if (impl->fxaaEnabled && impl->fxaaInitialized && impl->fxaaPipeline && pie.offscreenTexture)
            {
                MTLRenderPassDescriptor* fxaaPass = [MTLRenderPassDescriptor renderPassDescriptor];
                fxaaPass.colorAttachments[0].texture = pie.colorTexture;
                fxaaPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                fxaaPass.colorAttachments[0].storeAction = MTLStoreActionStore;

                id<MTLRenderCommandEncoder> fxaaEncoder =
                    [impl->commandBuffer renderCommandEncoderWithDescriptor:fxaaPass];
                if (fxaaEncoder) {
                    fxaaEncoder.label = @"FXAA PIE Viewport Encoder";

                    [fxaaEncoder setRenderPipelineState:impl->fxaaPipeline];

                    MTLViewport viewport = {0, 0, (double)pie.width, (double)pie.height, 0, 1};
                    [fxaaEncoder setViewport:viewport];

                    [fxaaEncoder setFragmentTexture:pie.offscreenTexture atIndex:0];
                    [fxaaEncoder setFragmentSamplerState:impl->defaultSampler atIndex:0];

                    MetalFXAAUniforms fxaaUniforms;
                    fxaaUniforms.inverseScreenSize = glm::vec2(
                        1.0f / std::max(pie.width, 1), 1.0f / std::max(pie.height, 1));
                    [fxaaEncoder setFragmentBytes:&fxaaUniforms length:sizeof(fxaaUniforms) atIndex:0];

                    [fxaaEncoder setVertexBuffer:impl->fxaaVertexBuffer offset:0 atIndex:0];
                    [fxaaEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

                    [fxaaEncoder endEncoding];
                }
            }
            // If FXAA is disabled, scene was rendered directly to PIE colorTexture — nothing to do
        }

        // Reset active scene target back to main viewport
        impl->activeSceneTarget = -1;

        // Reset bind tracking
        impl->lastBoundPipeline = nil;
        impl->lastBoundDepthStencil = nil;
        impl->lastBoundVertexBuffer = nil;
        impl->lastBoundTextureHandle = INVALID_TEXTURE;
        impl->perFrameUBOReady = false;
        return;
    }

    if (!impl->viewportTexture) return;

    // End the main scene encoder
    if (impl->mainPassActive && impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }

    // Apply FXAA from offscreen -> viewportTexture
    if (impl->fxaaEnabled && impl->fxaaInitialized && impl->fxaaPipeline && impl->offscreenTexture)
    {
        MTLRenderPassDescriptor* fxaaPass = [MTLRenderPassDescriptor renderPassDescriptor];
        fxaaPass.colorAttachments[0].texture = impl->viewportTexture;
        fxaaPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        fxaaPass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> fxaaEncoder =
            [impl->commandBuffer renderCommandEncoderWithDescriptor:fxaaPass];
        if (fxaaEncoder) {
            fxaaEncoder.label = @"FXAA Viewport Encoder";

            [fxaaEncoder setRenderPipelineState:impl->fxaaPipeline];

            MTLViewport viewport = {0, 0,
                (double)impl->viewportWidthRT, (double)impl->viewportHeightRT, 0, 1};
            [fxaaEncoder setViewport:viewport];

            [fxaaEncoder setFragmentTexture:impl->offscreenTexture atIndex:0];
            [fxaaEncoder setFragmentSamplerState:impl->defaultSampler atIndex:0];

            MetalFXAAUniforms fxaaUniforms;
            fxaaUniforms.inverseScreenSize = glm::vec2(
                1.0f / impl->viewportWidthRT, 1.0f / impl->viewportHeightRT);
            [fxaaEncoder setFragmentBytes:&fxaaUniforms length:sizeof(fxaaUniforms) atIndex:0];

            [fxaaEncoder setVertexBuffer:impl->fxaaVertexBuffer offset:0 atIndex:0];
            [fxaaEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

            [fxaaEncoder endEncoding];
        }
    }
    // If FXAA is disabled, scene was rendered directly to viewportTexture — nothing to do
}

uint64_t MetalRenderAPI::getViewportTextureID()
{
    if (!impl->viewportTexture) return 0;
    return (uint64_t)((__bridge void*)impl->viewportTexture);
}

// ── Preview render target (asset preview panel) ─────────────────────────────

void MetalRenderAPI::beginPreviewFrame(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (!impl->commandBuffer) return;

    // Recreate if size changed
    if (width != impl->previewWidthRT || height != impl->previewHeightRT || !impl->previewTexture)
    {
        impl->previewTexture = nil;
        impl->previewDepthTexture = nil;
        impl->previewWidthRT = width;
        impl->previewHeightRT = height;

        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:width
                                                                                      height:height
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        impl->previewTexture = [impl->device newTextureWithDescriptor:desc];

        impl->previewDepthTexture = impl->createDepthTextureWithSize(width, height);
    }

    if (!impl->previewTexture) return;

    // End any current encoder
    if (impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }

    // Create preview render pass
    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    passDesc.colorAttachments[0].texture = impl->previewTexture;
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.12, 0.12, 0.14, 1.0);
    passDesc.depthAttachment.texture = impl->previewDepthTexture;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
    passDesc.depthAttachment.clearDepth = 1.0;

    impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    if (!impl->encoder) return;
    impl->encoder.label = @"Preview Render Encoder";

    // Set viewport
    MTLViewport vp = { 0, 0, (double)width, (double)height, 0.0, 1.0 };
    [impl->encoder setViewport:vp];

    // Bind pipeline and buffers
    [impl->encoder setRenderPipelineState:impl->basicPipeline];
    [impl->encoder setDepthStencilState:impl->depthLessEqual];
    [impl->encoder setCullMode:MTLCullModeBack];
    [impl->encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    // Reset model matrix stack
    impl->modelMatrixStack = std::stack<glm::mat4>();
    impl->modelMatrixStack.push(glm::mat4(1.0f));
}

void MetalRenderAPI::endPreviewFrame()
{
    if (impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }
}

uint64_t MetalRenderAPI::getPreviewTextureID()
{
    if (!impl->previewTexture) return 0;
    return (uint64_t)((__bridge void*)impl->previewTexture);
}

void MetalRenderAPI::destroyPreviewTarget()
{
    impl->previewTexture = nil;
    impl->previewDepthTexture = nil;
    impl->previewWidthRT = 0;
    impl->previewHeightRT = 0;
}

// ── PIE viewport render targets (multi-player Play-In-Editor) ──────────────

int MetalRenderAPI::createPIEViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return -1;

    int id = impl->nextPIEId++;
    auto& target = impl->pieViewports[id];
    impl->createPIEViewportTextures(target, width, height);

    if (!target.colorTexture || !target.depthTexture)
    {
        impl->pieViewports.erase(id);
        return -1;
    }

    return id;
}

void MetalRenderAPI::destroyPIEViewport(int id)
{
    auto it = impl->pieViewports.find(id);
    if (it == impl->pieViewports.end()) return;

    it->second.colorTexture = nil;
    it->second.depthTexture = nil;
    it->second.offscreenTexture = nil;
    it->second.offscreenDepthTexture = nil;

    impl->pieViewports.erase(it);

    // If we just destroyed the active target, reset to main viewport
    if (impl->activeSceneTarget == id)
        impl->activeSceneTarget = -1;
}

void MetalRenderAPI::destroyAllPIEViewports()
{
    impl->pieViewports.clear();
    impl->activeSceneTarget = -1;
}

void MetalRenderAPI::setPIEViewportSize(int id, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    auto it = impl->pieViewports.find(id);
    if (it == impl->pieViewports.end()) return;

    if (it->second.width == width && it->second.height == height) return;

    impl->createPIEViewportTextures(it->second, width, height);
}

void MetalRenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    impl->activeSceneTarget = pie_viewport_id;
}

uint64_t MetalRenderAPI::getPIEViewportTextureID(int id)
{
    auto it = impl->pieViewports.find(id);
    if (it == impl->pieViewports.end() || !it->second.colorTexture) return 0;
    return (uint64_t)((__bridge void*)it->second.colorTexture);
}

void MetalRenderAPI::renderUI()
{
    if (!impl->viewportTexture) return;  // Not in editor mode
    if (!impl->commandBuffer) return;

    // Acquire drawable now (deferred from beginFrame/endShadowPass)
    if (!impl->ensureDrawable()) {
        printf("[Metal] renderUI: Failed to acquire drawable\n");
        return;
    }

    // Create UI render pass targeting the screen drawable
    MTLRenderPassDescriptor* uiPass = [MTLRenderPassDescriptor renderPassDescriptor];
    uiPass.colorAttachments[0].texture = impl->currentDrawable.texture;
    uiPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    uiPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    uiPass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);

    // Tell ImGui Metal backend about the UI render pass (for pipeline state selection)
    ImGui_ImplMetal_NewFrame(uiPass);

    id<MTLRenderCommandEncoder> uiEncoder =
        [impl->commandBuffer renderCommandEncoderWithDescriptor:uiPass];
    if (!uiEncoder) {
        printf("[Metal] renderUI: Failed to create UI encoder\n");
        return;
    }
    uiEncoder.label = @"UI Render Encoder";

    // Render ImGui draw data
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData && drawData->TotalVtxCount > 0) {
        ImGui_ImplMetal_RenderDrawData(drawData, impl->commandBuffer, uiEncoder);
    }

    [uiEncoder endEncoding];

    // Present and commit
    [impl->commandBuffer presentDrawable:impl->currentDrawable];

    // Signal semaphore on completion and log GPU errors
    __block dispatch_semaphore_t sem = impl->frameSemaphore;
    __block uint32_t frameNum = impl->frameNumber;
    __block uint32_t* errorCountPtr = &impl->gpuErrorCount;
    __block id<MTLDevice> dev = impl->device;
    __block id<MTLCommandQueue> __strong* queuePtr = &impl->commandQueue;
    [impl->commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buf) {
        if (buf.status == MTLCommandBufferStatusError) {
            (*errorCountPtr)++;
            printf("[Metal] GPU Error (frame %u, total errors: %u): %s\n",
                   frameNum, *errorCountPtr, [[buf.error localizedDescription] UTF8String]);
            if (@available(macOS 11.0, *)) {
                NSArray<id<MTLCommandBufferEncoderInfo>>* encoderInfos =
                    buf.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
                for (id<MTLCommandBufferEncoderInfo> info in encoderInfos) {
                    NSString* statusStr = @"unknown";
                    switch (info.errorState) {
                        case MTLCommandEncoderErrorStateCompleted: statusStr = @"completed"; break;
                        case MTLCommandEncoderErrorStateAffected: statusStr = @"affected"; break;
                        case MTLCommandEncoderErrorStateFaulted: statusStr = @"FAULTED"; break;
                        case MTLCommandEncoderErrorStatePending: statusStr = @"pending"; break;
                        default: break;
                    }
                    printf("[Metal]   Encoder '%s': %s\n",
                           [info.label UTF8String], [statusStr UTF8String]);
                }
            }
            fflush(stdout);
            if (*errorCountPtr >= MetalRenderAPIImpl::MAX_GPU_ERRORS_BEFORE_RECOVERY) {
                printf("[Metal] Too many consecutive GPU errors, recreating command queue\n");
                *queuePtr = [dev newCommandQueue];
                *errorCountPtr = 0;
            }
        } else {
            *errorCountPtr = 0;
        }
        dispatch_semaphore_signal(sem);
    }];

    [impl->commandBuffer commit];

    // Reset frame state
    impl->commandBuffer = nil;
    impl->currentDrawable = nil;
    impl->frameStarted = false;
    impl->frameNumber++;
    impl->currentFrame = (impl->currentFrame + 1) % MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT;
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
