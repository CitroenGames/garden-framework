#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include "MetalSceneViewport.hpp"
#include "MetalMesh.hpp"

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

    MTLTextureDescriptor* hdrDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
    hdrDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    hdrDesc.storageMode = MTLStorageModePrivate;
    deferredHDRTexture = [device newTextureWithDescriptor:hdrDesc];
    deferredHDRTexture.label = @"Metal Deferred HDR";

    MTLTextureDescriptor* gb0Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
    gb0Desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    gb0Desc.storageMode = MTLStorageModePrivate;
    gbuffer0Texture = [device newTextureWithDescriptor:gb0Desc];
    gbuffer0Texture.label = @"Metal GBuffer0";

    MTLTextureDescriptor* gbDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
    gbDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    gbDesc.storageMode = MTLStorageModePrivate;
    gbuffer1Texture = [device newTextureWithDescriptor:gbDesc];
    gbuffer1Texture.label = @"Metal GBuffer1";
    gbuffer2Texture = [device newTextureWithDescriptor:gbDesc];
    gbuffer2Texture.label = @"Metal GBuffer2";

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

    createSSAOResources(w, h);
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
    target.hdrTexture = nil;
    target.gbuffer0Texture = nil;
    target.gbuffer1Texture = nil;
    target.gbuffer2Texture = nil;
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

    MTLTextureDescriptor* hdrDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
    hdrDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    hdrDesc.storageMode = MTLStorageModePrivate;
    target.hdrTexture = [device newTextureWithDescriptor:hdrDesc];

    MTLTextureDescriptor* gb0Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
    gb0Desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    gb0Desc.storageMode = MTLStorageModePrivate;
    target.gbuffer0Texture = [device newTextureWithDescriptor:gb0Desc];

    MTLTextureDescriptor* gbDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
    gbDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    gbDesc.storageMode = MTLStorageModePrivate;
    target.gbuffer1Texture = [device newTextureWithDescriptor:gbDesc];
    target.gbuffer2Texture = [device newTextureWithDescriptor:gbDesc];
}

// ============================================================================
// Viewport rendering (for editor)
// ============================================================================

void MetalRenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    if (impl->editorSceneViewport)
    {
        impl->editorSceneViewport->resize(width, height);
        impl->viewportWidthRT = width;
        impl->viewportHeightRT = height;

        float ratio = (float)width / (float)height;
        impl->projectionMatrix = glm::perspectiveRH_ZO(glm::radians(impl->fieldOfView), ratio, 0.1f, 1000.0f);
        return;
    }

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
    if (isDeferredActive())
    {
        const bool explicitSceneTarget = impl->activeSceneTarget >= 0;
        impl->renderDeferredSceneToCurrentTarget(/*presentToDrawable=*/false,
                                                 /*renderImGui=*/false);
        if (explicitSceneTarget)
            impl->activeSceneTarget = -1;
        return;
    }

    int sceneTarget = impl->activeSceneTarget;
    if (sceneTarget < 0 && impl->editorSceneViewport)
        sceneTarget = impl->editorSceneViewport->pieId();

    // Check if we are rendering to a PIE/editor SceneViewport target.
    if (sceneTarget >= 0)
    {
        auto it = impl->pieViewports.find(sceneTarget);
        if (it != impl->pieViewports.end())
        {
            auto& pie = it->second;

            // End the main scene encoder
            if (impl->mainPassActive && impl->encoder) {
                [impl->encoder endEncoding];
                impl->encoder = nil;
                impl->mainPassActive = false;
            }

            id<MTLTexture> ssaoTexture = nil;
            if (impl->fxaaEnabled && impl->fxaaInitialized && pie.offscreenDepthTexture) {
                ssaoTexture = impl->runSSAOPasses(pie.offscreenDepthTexture, pie.width, pie.height);
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
                    id<MTLTexture> aoTexture = ssaoTexture ? ssaoTexture : impl->ssaoFallbackTexture;
                    if (aoTexture) {
                        [fxaaEncoder setFragmentTexture:aoTexture atIndex:1];
                    }
                    [fxaaEncoder setFragmentSamplerState:impl->defaultSampler atIndex:0];

                    MetalFXAAUniforms fxaaUniforms;
                    fxaaUniforms.inverseScreenSize = glm::vec2(
                        1.0f / std::max(pie.width, 1), 1.0f / std::max(pie.height, 1));
                    fxaaUniforms.exposure = 1.0f;
                    fxaaUniforms.ssaoEnabled = ssaoTexture ? 1 : 0;
                    fxaaUniforms.fxaaEnabled = impl->fxaaEnabled ? 1 : 0;
                    [fxaaEncoder setFragmentBytes:&fxaaUniforms length:sizeof(fxaaUniforms) atIndex:0];

                    [fxaaEncoder setVertexBuffer:impl->fxaaVertexBuffer offset:0 atIndex:0];
                    [fxaaEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

                    [fxaaEncoder endEncoding];
                }
            }
            // If FXAA is disabled, scene was rendered directly to PIE colorTexture — nothing to do
        }

        // Reset explicit scene target back to the currently bound editor viewport.
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

    id<MTLTexture> ssaoTexture = nil;
    if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenDepthTexture) {
        ssaoTexture = impl->runSSAOPasses(impl->offscreenDepthTexture,
                                          impl->viewportWidthRT, impl->viewportHeightRT);
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
            id<MTLTexture> aoTexture = ssaoTexture ? ssaoTexture : impl->ssaoFallbackTexture;
            if (aoTexture) {
                [fxaaEncoder setFragmentTexture:aoTexture atIndex:1];
            }
            [fxaaEncoder setFragmentSamplerState:impl->defaultSampler atIndex:0];

            MetalFXAAUniforms fxaaUniforms;
            fxaaUniforms.inverseScreenSize = glm::vec2(
                1.0f / impl->viewportWidthRT, 1.0f / impl->viewportHeightRT);
            fxaaUniforms.exposure = 1.0f;
            fxaaUniforms.ssaoEnabled = ssaoTexture ? 1 : 0;
            fxaaUniforms.fxaaEnabled = impl->fxaaEnabled ? 1 : 0;
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
    if (impl->editorSceneViewport)
        return impl->editorSceneViewport->getOutputTextureID();
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
    it->second.hdrTexture = nil;
    it->second.gbuffer0Texture = nil;
    it->second.gbuffer1Texture = nil;
    it->second.gbuffer2Texture = nil;

    impl->pieViewports.erase(it);

    // If we just destroyed the active target, reset to main viewport
    if (impl->activeSceneTarget == id)
        impl->activeSceneTarget = -1;
    if (impl->editorSceneViewport && impl->editorSceneViewport->pieId() == id)
        impl->editorSceneViewport = nullptr;
}

void MetalRenderAPI::destroyAllPIEViewports()
{
    impl->pieViewports.clear();
    impl->activeSceneTarget = -1;
    impl->editorSceneViewport = nullptr;
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

std::unique_ptr<SceneViewport> MetalRenderAPI::createSceneViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return nullptr;

    auto viewport = std::make_unique<MetalSceneViewport>(this, width, height);
    if (viewport->pieId() < 0)
        return nullptr;
    return viewport;
}

void MetalRenderAPI::setEditorViewport(SceneViewport* viewport)
{
    auto* metalViewport = static_cast<MetalSceneViewport*>(viewport);
    impl->editorSceneViewport = metalViewport;
    impl->activeSceneTarget = metalViewport ? metalViewport->pieId() : -1;
}

// ============================================================================
// Deferred rendering path
// ============================================================================

namespace {

struct MetalDeferredTarget {
    id<MTLTexture> output = nil;
    id<MTLTexture> hdr = nil;
    id<MTLTexture> depth = nil;
    id<MTLTexture> gb0 = nil;
    id<MTLTexture> gb1 = nil;
    id<MTLTexture> gb2 = nil;
    int width = 0;
    int height = 0;
};

static MetalDeferredTarget getDeferredTarget(MetalRenderAPIImpl* impl, bool presentToDrawable)
{
    MetalDeferredTarget target;

    int sceneTarget = impl->activeSceneTarget;
    if (sceneTarget < 0 && impl->editorSceneViewport)
        sceneTarget = impl->editorSceneViewport->pieId();

    if (sceneTarget >= 0) {
        auto it = impl->pieViewports.find(sceneTarget);
        if (it != impl->pieViewports.end()) {
            auto& pie = it->second;
            target.output = pie.colorTexture;
            target.hdr = pie.hdrTexture;
            target.depth = pie.offscreenDepthTexture ? pie.offscreenDepthTexture : pie.depthTexture;
            target.gb0 = pie.gbuffer0Texture;
            target.gb1 = pie.gbuffer1Texture;
            target.gb2 = pie.gbuffer2Texture;
            target.width = pie.width;
            target.height = pie.height;
            return target;
        }
    }

    target.output = presentToDrawable && impl->currentDrawable ? impl->currentDrawable.texture : impl->viewportTexture;
    target.hdr = impl->deferredHDRTexture;
    target.depth = impl->offscreenDepthTexture;
    target.gb0 = impl->gbuffer0Texture;
    target.gb1 = impl->gbuffer1Texture;
    target.gb2 = impl->gbuffer2Texture;
    target.width = impl->viewportWidthRT > 0 ? impl->viewportWidthRT : impl->viewportWidth;
    target.height = impl->viewportHeightRT > 0 ? impl->viewportHeightRT : impl->viewportHeight;
    return target;
}

static bool targetReady(const MetalDeferredTarget& target)
{
    return target.output && target.hdr && target.depth && target.gb0 && target.gb1 && target.gb2
        && target.width > 0 && target.height > 0;
}

static void bindDrawTexture(MetalRenderAPIImpl* impl,
                            id<MTLRenderCommandEncoder> enc,
                            const DrawCommand& drawCmd)
{
    TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
    if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
        auto& tex = impl->textures[texHandle];
        [enc setFragmentTexture:tex.texture atIndex:0];
        [enc setFragmentSamplerState:tex.sampler atIndex:0];
    } else {
        [enc setFragmentTexture:impl->defaultTexture atIndex:0];
        [enc setFragmentSamplerState:impl->defaultSampler atIndex:0];
    }
}

static void drawCommandMesh(MetalRenderAPIImpl* impl,
                            id<MTLRenderCommandEncoder> enc,
                            const DrawCommand& drawCmd)
{
    MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(drawCmd.gpu_mesh);
    if (!metalMesh) return;

    id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
    if (!vertexBuffer) return;

    [enc setVertexBuffer:vertexBuffer offset:0 atIndex:0];

    if (metalMesh->isIndexed()) {
        id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
        if (!indexBuffer) return;
        if (drawCmd.vertex_count > 0) {
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:drawCmd.vertex_count
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:indexBuffer
                     indexBufferOffset:drawCmd.start_vertex * sizeof(uint32_t)];
        } else {
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:metalMesh->getIndexCount()
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:indexBuffer
                     indexBufferOffset:0];
        }
    } else {
        if (drawCmd.vertex_count > 0) {
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:drawCmd.start_vertex
                    vertexCount:drawCmd.vertex_count];
        } else {
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:metalMesh->getVertexCount()];
        }
    }

    impl->drawCallCount++;
    impl->lastFrameStats.backend_draw_calls++;
}

static void encodeCommands(MetalRenderAPIImpl* impl,
                           id<MTLRenderCommandEncoder> enc,
                           const RenderCommandBuffer& cmds,
                           id<MTLRenderPipelineState> forcedPipeline,
                           bool hdrForward)
{
    if (cmds.empty()) return;

    impl->lastFrameStats.submitted_draw_commands += cmds.size();
    impl->updatePerFrameUBO();
    MetalGlobalUBO frameUBO = impl->cachedPerFrameUBO;

    if (!forcedPipeline) {
        impl->bindLightBuffers(enc);
        if (impl->shadowMapArray) {
            [enc setFragmentTexture:impl->shadowMapArray atIndex:1];
            [enc setFragmentSamplerState:impl->shadowSampler atIndex:1];
        }
    }

    for (const auto& drawCmd : cmds) {
        if (!drawCmd.gpu_mesh || !drawCmd.gpu_mesh->isUploaded()) continue;

        id<MTLRenderPipelineState> pipeline = forcedPipeline;
        if (!pipeline)
            pipeline = hdrForward ? impl->selectHDRPipeline(drawCmd.pso_key)
                                  : impl->selectPipeline(drawCmd.pso_key);
        if (!pipeline) continue;

        [enc setRenderPipelineState:pipeline];

        MTLCullMode cullMode = MTLCullModeBack;
        switch (drawCmd.pso_key.cull) {
            case CullMode::Back:  cullMode = MTLCullModeBack; break;
            case CullMode::Front: cullMode = MTLCullModeFront; break;
            case CullMode::None:  cullMode = MTLCullModeNone; break;
        }
        [enc setCullMode:cullMode];

        if (forcedPipeline || drawCmd.pso_key.blend == BlendMode::None)
            [enc setDepthStencilState:impl->depthLessEqual];
        else
            [enc setDepthStencilState:impl->depthLessEqualNoWrite];

        MetalGlobalUBO ubo = frameUBO;
        ubo.color = drawCmd.color;
        ubo.useTexture = drawCmd.use_texture ? 1 : 0;
        ubo.alphaCutoff = drawCmd.alpha_cutoff;
        ubo.metallic = drawCmd.metallic;
        ubo.roughness = drawCmd.roughness;
        ubo.emissive = drawCmd.emissive;
        [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
        [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];

        struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
        modelData.model = drawCmd.model_matrix;
        modelData.normalMatrix = drawCmd.normal_matrix;
        [enc setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

        bindDrawTexture(impl, enc, drawCmd);
        drawCommandMesh(impl, enc, drawCmd);
    }
}

static void encodeDebugLinesToHDR(MetalRenderAPIImpl* impl,
                                  id<MTLTexture> hdr,
                                  id<MTLTexture> depth,
                                  int width,
                                  int height)
{
    if (impl->deferredDebugLineVertices.size() < 2 || !impl->debugLineHDRPipeline)
        return;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = hdr;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc = [impl->commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (!enc) return;
    enc.label = @"Deferred Debug Lines";
    [enc setRenderPipelineState:impl->debugLineHDRPipeline];
    [enc setDepthStencilState:impl->depthLessEqual];
    [enc setCullMode:MTLCullModeNone];
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
    MTLViewport vp = {0, 0, (double)width, (double)height, 0, 1};
    [enc setViewport:vp];

    impl->updatePerFrameUBO();
    MetalGlobalUBO ubo = impl->cachedPerFrameUBO;
    glm::mat4 identity(1.0f);
    [enc setVertexBytes:&identity length:sizeof(glm::mat4) atIndex:2];
    [enc setFragmentTexture:impl->defaultTexture atIndex:0];
    [enc setFragmentSamplerState:impl->defaultSampler atIndex:0];

    size_t i = 0;
    const auto& vertices = impl->deferredDebugLineVertices;
    while (i < vertices.size()) {
        glm::vec3 color(vertices[i].nx, vertices[i].ny, vertices[i].nz);
        size_t batchStart = i;
        while (i < vertices.size()
            && vertices[i].nx == color.r
            && vertices[i].ny == color.g
            && vertices[i].nz == color.b) {
            ++i;
        }

        ubo.color = color;
        ubo.useTexture = 0;
        [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
        [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
        [enc setVertexBytes:vertices.data() + batchStart
                     length:(i - batchStart) * sizeof(vertex)
                    atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:i - batchStart];
    }

    [enc endEncoding];
}

static void commitDeferredFrame(MetalRenderAPIImpl* impl, bool presentToDrawable)
{
    if (presentToDrawable && impl->currentDrawable)
        [impl->commandBuffer presentDrawable:impl->currentDrawable];

    __block dispatch_semaphore_t sem = impl->frameSemaphore;
    __block uint32_t frameNum = impl->frameNumber;
    __block uint32_t* errorCountPtr = &impl->gpuErrorCount;
    __block id<MTLDevice> dev = impl->device;
    __block id<MTLCommandQueue> __strong* queuePtr = &impl->commandQueue;
    [impl->commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buf) {
        if (buf.status == MTLCommandBufferStatusError) {
            (*errorCountPtr)++;
            printf("[Metal] GPU Error (deferred frame %u, total errors: %u): %s\n",
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
    impl->commandBuffer = nil;
    impl->currentDrawable = nil;
    impl->frameStarted = false;
    impl->frameNumber++;
    impl->currentFrame = (impl->currentFrame + 1) % MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT;
}

} // namespace

void MetalRenderAPIImpl::renderDeferredSceneToCurrentTarget(bool presentToDrawable, bool renderImGui)
{
    if (!frameStarted || !commandBuffer)
        return;

    if (presentToDrawable && !ensureDrawable())
        return;

    MetalDeferredTarget target = getDeferredTarget(this, presentToDrawable);
    if (presentToDrawable && currentDrawable)
        target.output = currentDrawable.texture;
    if (!targetReady(target))
        return;

    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target.gb0;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[1].texture = target.gb1;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
        pass.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[2].texture = target.gb2;
        pass.colorAttachments[2].loadAction = MTLLoadActionClear;
        pass.colorAttachments[2].storeAction = MTLStoreActionStore;
        pass.colorAttachments[2].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.depthAttachment.texture = target.depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (enc) {
            enc.label = @"GBuffer Encoder";
            MTLViewport vp = {0, 0, (double)target.width, (double)target.height, 0, 1};
            [enc setViewport:vp];
            [enc setFrontFacingWinding:MTLWindingCounterClockwise];
            encodeCommands(this, enc, deferredOpaqueCmds, gbufferPipeline, false);
            [enc endEncoding];
        }
    }

    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target.hdr;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (enc) {
            enc.label = @"Deferred Lighting Encoder";
            [enc setRenderPipelineState:deferredLightingPipeline];
            MTLViewport vp = {0, 0, (double)target.width, (double)target.height, 0, 1};
            [enc setViewport:vp];

            MetalDeferredLightingUniforms ubo{};
            ubo.invViewProj = glm::inverse(projectionMatrix * viewMatrix);
            ubo.view = viewMatrix;
            for (int i = 0; i < NUM_CASCADES; ++i)
                ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
            ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                          cascadeSplitDistances[2], cascadeSplitDistances[3]);
            ubo.cascadeSplit4 = cascadeSplitDistances[4];
            ubo.cascadeCount = shadowMapArray ? std::clamp(activeCascadeCount, 1, NUM_CASCADES) : 0;
            ubo.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(shadowMapSize));
            const glm::mat4 invView = glm::inverse(viewMatrix);
            ubo.cameraPos = glm::vec3(invView[3]);
            ubo.lightDir = lightDirection;
            ubo.lightAmbient = lightAmbient;
            ubo.lightDiffuse = lightDiffuse;
            ubo.numPointLights = std::clamp(numPointLightsUploaded, 0, MAX_LIGHTS_DEFERRED);
            ubo.numSpotLights = std::clamp(numSpotLightsUploaded, 0, MAX_LIGHTS_DEFERRED);

            [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
            if (pointLightBuffers[currentFrame])
                [enc setFragmentBuffer:pointLightBuffers[currentFrame] offset:0 atIndex:1];
            if (spotLightBuffers[currentFrame])
                [enc setFragmentBuffer:spotLightBuffers[currentFrame] offset:0 atIndex:2];
            [enc setFragmentTexture:target.gb0 atIndex:0];
            [enc setFragmentTexture:target.gb1 atIndex:1];
            [enc setFragmentTexture:target.gb2 atIndex:2];
            [enc setFragmentTexture:target.depth atIndex:3];
            if (shadowMapArray)
                [enc setFragmentTexture:shadowMapArray atIndex:4];
            [enc setFragmentSamplerState:defaultSampler atIndex:0];
            id<MTLSamplerState> depthSampler = ssaoDepthSampler ? ssaoDepthSampler : defaultSampler;
            [enc setFragmentSamplerState:depthSampler atIndex:1];
            if (shadowSampler)
                [enc setFragmentSamplerState:shadowSampler atIndex:2];
            [enc setVertexBuffer:fxaaVertexBuffer offset:0 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [enc endEncoding];
        }
    }

    if (skyboxRequested && skyboxHDRPipeline && skyboxVertexBuffer) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target.hdr;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = target.depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (enc) {
            enc.label = @"Deferred Skybox Encoder";
            [enc setRenderPipelineState:skyboxHDRPipeline];
            [enc setDepthStencilState:depthLessEqualNoWrite];
            [enc setCullMode:MTLCullModeNone];
            MTLViewport vp = {0, 0, (double)target.width, (double)target.height, 0, 1};
            [enc setViewport:vp];

            MetalSkyboxUBO skyUBO;
            skyUBO.view = viewMatrix;
            skyUBO.projection = projectionMatrix;
            skyUBO.sunDirection = -lightDirection;
            skyUBO.time = 0.0f;
            [enc setVertexBytes:&skyUBO length:sizeof(skyUBO) atIndex:1];
            [enc setFragmentBytes:&skyUBO length:sizeof(skyUBO) atIndex:0];
            [enc setVertexBuffer:skyboxVertexBuffer offset:0 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
            [enc endEncoding];
        }
    }

    if (!deferredTransparentCmds.empty()) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target.hdr;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = target.depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (enc) {
            enc.label = @"Deferred Transparent Forward";
            MTLViewport vp = {0, 0, (double)target.width, (double)target.height, 0, 1};
            [enc setViewport:vp];
            [enc setFrontFacingWinding:MTLWindingCounterClockwise];
            encodeCommands(this, enc, deferredTransparentCmds, nil, true);
            [enc endEncoding];
        }
    }

    encodeDebugLinesToHDR(this, target.hdr, target.depth, target.width, target.height);

    id<MTLTexture> ssaoTexture = nil;
    if (ssaoEnabled && target.depth)
        ssaoTexture = runSSAOPasses(target.depth, target.width, target.height);

    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target.output;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        if (renderImGui)
            ImGui_ImplMetal_NewFrame(pass);

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (enc) {
            enc.label = @"Deferred Resolve Encoder";
            [enc setRenderPipelineState:fxaaPipeline];
            MTLViewport vp = {0, 0, (double)target.width, (double)target.height, 0, 1};
            [enc setViewport:vp];
            [enc setFragmentTexture:target.hdr atIndex:0];
            id<MTLTexture> aoTexture = ssaoTexture ? ssaoTexture : ssaoFallbackTexture;
            if (aoTexture)
                [enc setFragmentTexture:aoTexture atIndex:1];
            [enc setFragmentSamplerState:defaultSampler atIndex:0];
            MetalFXAAUniforms fxaaUniforms{};
            fxaaUniforms.inverseScreenSize = glm::vec2(1.0f / target.width, 1.0f / target.height);
            fxaaUniforms.exposure = 1.0f;
            fxaaUniforms.ssaoEnabled = ssaoTexture ? 1 : 0;
            fxaaUniforms.fxaaEnabled = fxaaEnabled ? 1 : 0;
            [enc setFragmentBytes:&fxaaUniforms length:sizeof(fxaaUniforms) atIndex:0];
            [enc setVertexBuffer:fxaaVertexBuffer offset:0 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

            if (renderImGui) {
                ImDrawData* drawData = ImGui::GetDrawData();
                if (drawData && drawData->TotalVtxCount > 0)
                    ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, enc);
            }
            [enc endEncoding];
        }
    }

    deferredOpaqueCmds.clear();
    deferredTransparentCmds.clear();
    deferredDebugLineVertices.clear();
    skyboxRequested = false;

    if (presentToDrawable)
        commitDeferredFrame(this, true);
}

// ============================================================================
// UI Rendering (editor mode)
// ============================================================================

void MetalRenderAPI::renderUI()
{
    if (!impl->viewportTexture && !impl->editorSceneViewport) return;  // Not in editor mode
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
