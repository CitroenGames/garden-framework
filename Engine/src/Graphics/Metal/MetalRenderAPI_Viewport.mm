#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"

// ============================================================================
// MetalRenderAPIImpl helper definitions (viewport/offscreen-related)
// ============================================================================

void MetalRenderAPIImpl::createOffscreenResources(int w, int h)
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

    // Create 1x1 white SSAO fallback texture (ensures FXAA can always sample texture(1))
    if (!ssaoFallbackTexture) {
        MTLTextureDescriptor* fbDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                         width:1
                                                                                        height:1
                                                                                     mipmapped:NO];
        fbDesc.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
        fbDesc.storageMode = MTLStorageModeManaged;
#else
        fbDesc.storageMode = MTLStorageModeShared;
#endif
        ssaoFallbackTexture = [device newTextureWithDescriptor:fbDesc];
        uint8_t white = 255;
        [ssaoFallbackTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                               mipmapLevel:0
                                 withBytes:&white
                               bytesPerRow:1];
    }
}

void MetalRenderAPIImpl::createOffscreenResources()
{
    createOffscreenResources(viewportWidth, viewportHeight);
}

void MetalRenderAPIImpl::createViewportResources(int w, int h)
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

void MetalRenderAPIImpl::createPIEViewportTextures(PIEViewportTarget& target, int w, int h)
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

// ============================================================================
// Preview render target (asset preview panel)
// ============================================================================

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

// ============================================================================
// PIE viewport render targets (multi-player Play-In-Editor)
// ============================================================================

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

// ============================================================================
// UI Rendering (editor mode)
// ============================================================================

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
