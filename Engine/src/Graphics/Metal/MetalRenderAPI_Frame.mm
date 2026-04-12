#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"

// ============================================================================
// Frame lifecycle
// ============================================================================

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
