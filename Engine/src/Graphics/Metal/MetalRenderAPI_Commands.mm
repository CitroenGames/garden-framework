#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include "MetalMesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "Utils/Vertex.hpp"
#include "Utils/Log.hpp"

#include <future>
#include <algorithm>

// ============================================================================
// Command Buffer Replay (single-threaded)
// ============================================================================

void MetalRenderAPI::replayCommandBuffer(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || !impl->frameStarted) return;

    id<MTLRenderCommandEncoder> enc = impl->encoder;
    if (!enc) return;

    // Build per-frame UBO once
    impl->updatePerFrameUBO();
    MetalGlobalUBO globalUBO = impl->cachedPerFrameUBO;

    // Upload LightCBuffer once
    [enc setFragmentBytes:&impl->currentLights length:sizeof(LightCBuffer) atIndex:3];

    // Bind shadow map once
    if (impl->shadowMapArray) {
        [enc setFragmentTexture:impl->shadowMapArray atIndex:1];
        [enc setFragmentSamplerState:impl->shadowSampler atIndex:1];
    }

    // Local redundant-bind tracking
    id<MTLRenderPipelineState> lastPipeline = nil;
    id<MTLBuffer> lastVB = nil;
    TextureHandle lastTex = INVALID_TEXTURE + 1; // force first bind
    MTLCullMode lastCull = (MTLCullMode)0xFF;     // sentinel

    for (const auto& drawCmd : cmds)
    {
        if (!drawCmd.gpu_mesh || !drawCmd.gpu_mesh->isUploaded()) continue;

        MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(drawCmd.gpu_mesh);
        if (!metalMesh) continue;
        id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
        if (!vertexBuffer) continue;

        // --- Shadow pass draw ---
        if (drawCmd.pso_key.shadow)
        {
            // Select alpha-test or opaque shadow pipeline
            if (drawCmd.pso_key.alpha_test && impl->shadowAlphaTestPipeline) {
                [enc setRenderPipelineState:impl->shadowAlphaTestPipeline];
                // Bind texture for alpha sampling
                TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
                if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
                    auto& tex = impl->textures[texHandle];
                    [enc setFragmentTexture:tex.texture atIndex:0];
                    [enc setFragmentSamplerState:tex.sampler atIndex:0];
                }
            }

            MetalShadowUBO shadowUBO;
            shadowUBO.lightSpaceMatrix = impl->lightSpaceMatrices[impl->currentCascade];
            [enc setVertexBytes:&shadowUBO length:sizeof(shadowUBO) atIndex:1];
            [enc setVertexBytes:&drawCmd.model_matrix length:sizeof(glm::mat4) atIndex:2];

            if (vertexBuffer != lastVB) {
                [enc setVertexBuffer:vertexBuffer offset:0 atIndex:0];
                lastVB = vertexBuffer;
            }

            if (metalMesh->isIndexed()) {
                id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
                if (drawCmd.vertex_count > 0)
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:drawCmd.vertex_count
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:indexBuffer
                             indexBufferOffset:drawCmd.start_vertex * sizeof(uint32_t)];
                else
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:metalMesh->getIndexCount()
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:indexBuffer
                             indexBufferOffset:0];
            } else {
                if (drawCmd.vertex_count > 0)
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:drawCmd.start_vertex
                            vertexCount:drawCmd.vertex_count];
                else
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:metalMesh->getVertexCount()];
            }

            // Restore opaque shadow pipeline if we switched to alpha-test
            if (drawCmd.pso_key.alpha_test && impl->shadowAlphaTestPipeline) {
                [enc setRenderPipelineState:impl->shadowPipeline];
            }

            impl->drawCallCount++;
            continue;
        }

        // --- Main pass / depth prepass draw ---

        // Select pipeline from PSOKey
        id<MTLRenderPipelineState> pipeline = impl->selectPipeline(drawCmd.pso_key);
        if (pipeline != lastPipeline) {
            [enc setRenderPipelineState:pipeline];
            lastPipeline = pipeline;
        }

        // Set cull mode
        MTLCullMode cullMode;
        switch (drawCmd.pso_key.cull) {
            case CullMode::Back:  cullMode = MTLCullModeBack; break;
            case CullMode::Front: cullMode = MTLCullModeFront; break;
            case CullMode::None:  cullMode = MTLCullModeNone; break;
        }
        if (cullMode != lastCull) {
            [enc setCullMode:cullMode];
            lastCull = cullMode;
        }

        // Set depth stencil state
        if (drawCmd.pso_key.depth_only) {
            [enc setDepthStencilState:impl->depthLessEqual];
        } else if (drawCmd.pso_key.blend == BlendMode::None) {
            [enc setDepthStencilState:impl->depthLessEqual];
        } else {
            // Transparent objects: depth test but no depth write
            [enc setDepthStencilState:impl->depthLessEqualNoWrite];
        }

        // Build per-draw UBO (copy per-frame, override per-draw fields)
        MetalGlobalUBO ubo = globalUBO;
        ubo.color = drawCmd.color;
        ubo.useTexture = drawCmd.use_texture ? 1 : 0;
        ubo.alphaCutoff = drawCmd.alpha_cutoff;

        [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
        [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];

        // Set model + normal matrix
        struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
        modelData.model = drawCmd.model_matrix;
        modelData.normalMatrix = glm::transpose(glm::inverse(drawCmd.model_matrix));
        [enc setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

        // Bind texture (only if changed)
        TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
        if (texHandle != lastTex) {
            if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
                auto& tex = impl->textures[texHandle];
                [enc setFragmentTexture:tex.texture atIndex:0];
                [enc setFragmentSamplerState:tex.sampler atIndex:0];
            } else {
                [enc setFragmentTexture:impl->defaultTexture atIndex:0];
                [enc setFragmentSamplerState:impl->defaultSampler atIndex:0];
            }
            lastTex = texHandle;
        }

        // Bind vertex buffer (only if changed)
        if (vertexBuffer != lastVB) {
            [enc setVertexBuffer:vertexBuffer offset:0 atIndex:0];
            lastVB = vertexBuffer;
        }

        // Issue draw call
        if (metalMesh->isIndexed()) {
            id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
            if (drawCmd.vertex_count > 0)
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:drawCmd.vertex_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:indexBuffer
                         indexBufferOffset:drawCmd.start_vertex * sizeof(uint32_t)];
            else
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:metalMesh->getIndexCount()
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:indexBuffer
                         indexBufferOffset:0];
        } else {
            if (drawCmd.vertex_count > 0)
                [enc drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:drawCmd.start_vertex
                        vertexCount:drawCmd.vertex_count];
            else
                [enc drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:metalMesh->getVertexCount()];
        }
        impl->drawCallCount++;
    }
}

// ============================================================================
// Helper: replay a range of commands on a given encoder
// ============================================================================

static void replayCommandRange(MetalRenderAPIImpl* impl,
                                id<MTLRenderCommandEncoder> enc,
                                const RenderCommandBuffer& cmds,
                                size_t start, size_t end,
                                const MetalGlobalUBO& globalUBO,
                                int rtWidth, int rtHeight)
{
    // Set up per-encoder state
    MTLViewport viewport = {0, 0, (double)rtWidth, (double)rtHeight, 0, 1};
    [enc setViewport:viewport];
    [enc setDepthStencilState:impl->depthLessEqual];
    [enc setCullMode:MTLCullModeBack];
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];

    // Bind shadow map
    if (impl->shadowMapArray) {
        [enc setFragmentTexture:impl->shadowMapArray atIndex:1];
        [enc setFragmentSamplerState:impl->shadowSampler atIndex:1];
    }

    // Bind lights once
    [enc setFragmentBytes:&impl->currentLights length:sizeof(LightCBuffer) atIndex:3];

    // Local bind tracking
    id<MTLRenderPipelineState> lastPipeline = nil;
    id<MTLBuffer> lastVB = nil;
    TextureHandle lastTex = INVALID_TEXTURE + 1;
    MTLCullMode lastCull = (MTLCullMode)0xFF;

    for (size_t i = start; i < end; i++)
    {
        const auto& drawCmd = cmds[i];
        if (!drawCmd.gpu_mesh || !drawCmd.gpu_mesh->isUploaded()) continue;

        MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(drawCmd.gpu_mesh);
        if (!metalMesh) continue;
        id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
        if (!vertexBuffer) continue;

        // Select pipeline
        id<MTLRenderPipelineState> pipeline = impl->selectPipeline(drawCmd.pso_key);
        if (pipeline != lastPipeline) {
            [enc setRenderPipelineState:pipeline];
            lastPipeline = pipeline;
        }

        // Set cull mode
        MTLCullMode cullMode;
        switch (drawCmd.pso_key.cull) {
            case CullMode::Back:  cullMode = MTLCullModeBack; break;
            case CullMode::Front: cullMode = MTLCullModeFront; break;
            case CullMode::None:  cullMode = MTLCullModeNone; break;
        }
        if (cullMode != lastCull) {
            [enc setCullMode:cullMode];
            lastCull = cullMode;
        }

        // Depth state
        if (drawCmd.pso_key.blend == BlendMode::None)
            [enc setDepthStencilState:impl->depthLessEqual];
        else
            [enc setDepthStencilState:impl->depthLessEqualNoWrite];

        // Per-draw UBO
        MetalGlobalUBO ubo = globalUBO;
        ubo.color = drawCmd.color;
        ubo.useTexture = drawCmd.use_texture ? 1 : 0;
        ubo.alphaCutoff = drawCmd.alpha_cutoff;
        [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
        [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];

        // Model + normal matrix
        struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
        modelData.model = drawCmd.model_matrix;
        modelData.normalMatrix = glm::transpose(glm::inverse(drawCmd.model_matrix));
        [enc setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

        // Texture
        TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
        if (texHandle != lastTex) {
            if (texHandle != INVALID_TEXTURE && impl->textures.count(texHandle)) {
                auto& tex = impl->textures[texHandle];
                [enc setFragmentTexture:tex.texture atIndex:0];
                [enc setFragmentSamplerState:tex.sampler atIndex:0];
            } else {
                [enc setFragmentTexture:impl->defaultTexture atIndex:0];
                [enc setFragmentSamplerState:impl->defaultSampler atIndex:0];
            }
            lastTex = texHandle;
        }

        // Vertex buffer
        if (vertexBuffer != lastVB) {
            [enc setVertexBuffer:vertexBuffer offset:0 atIndex:0];
            lastVB = vertexBuffer;
        }

        // Draw
        if (metalMesh->isIndexed()) {
            id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
            if (drawCmd.vertex_count > 0)
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:drawCmd.vertex_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:indexBuffer
                         indexBufferOffset:drawCmd.start_vertex * sizeof(uint32_t)];
            else
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:metalMesh->getIndexCount()
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:indexBuffer
                         indexBufferOffset:0];
        } else {
            if (drawCmd.vertex_count > 0)
                [enc drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:drawCmd.start_vertex
                        vertexCount:drawCmd.vertex_count];
            else
                [enc drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:metalMesh->getVertexCount()];
        }
    }

    [enc endEncoding];
}

// ============================================================================
// Helper: build render pass descriptor for current render target (load action)
// ============================================================================

static MTLRenderPassDescriptor* buildLoadPassDescriptor(MetalRenderAPIImpl* impl)
{
    bool editorMode = (impl->viewportTexture != nil);
    bool pieMode = false;
    MetalRenderAPIImpl::PIEViewportTarget* pieTarget = nullptr;
    if (impl->activeSceneTarget >= 0) {
        auto it = impl->pieViewports.find(impl->activeSceneTarget);
        if (it != impl->pieViewports.end() && it->second.colorTexture) {
            pieMode = true;
            pieTarget = &it->second;
        }
    }

    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];

    // Color attachment
    if (pieMode && pieTarget) {
        if (impl->fxaaEnabled && impl->fxaaInitialized && pieTarget->offscreenTexture)
            passDesc.colorAttachments[0].texture = pieTarget->offscreenTexture;
        else
            passDesc.colorAttachments[0].texture = pieTarget->colorTexture;
    } else if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenTexture) {
        passDesc.colorAttachments[0].texture = impl->offscreenTexture;
    } else if (editorMode) {
        passDesc.colorAttachments[0].texture = impl->viewportTexture;
    } else {
        passDesc.colorAttachments[0].texture = impl->currentDrawable.texture;
    }
    passDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

    // Depth attachment
    id<MTLTexture> depthTex;
    if (pieMode && pieTarget) {
        if (impl->fxaaEnabled && impl->fxaaInitialized && pieTarget->offscreenDepthTexture)
            depthTex = pieTarget->offscreenDepthTexture;
        else
            depthTex = pieTarget->depthTexture;
    } else if (impl->fxaaEnabled && impl->fxaaInitialized && impl->offscreenDepthTexture) {
        depthTex = impl->offscreenDepthTexture;
    } else if (editorMode) {
        depthTex = impl->viewportDepthTexture;
    } else {
        depthTex = impl->depthTexture;
    }
    passDesc.depthAttachment.texture = depthTex;
    passDesc.depthAttachment.loadAction = MTLLoadActionLoad;
    passDesc.depthAttachment.storeAction = MTLStoreActionStore;

    return passDesc;
}

// ============================================================================
// Command Buffer Replay (multi-threaded via MTLParallelRenderCommandEncoder)
// ============================================================================

static constexpr size_t METAL_PARALLEL_THRESHOLD = 512;

void MetalRenderAPI::replayCommandBufferParallel(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || !impl->frameStarted) return;

    // Fall back for small buffers or missing encoder
    if (cmds.size() < METAL_PARALLEL_THRESHOLD || !impl->encoder) {
        replayCommandBuffer(cmds);
        return;
    }

    // Determine render target dimensions
    bool editorMode = (impl->viewportTexture != nil);
    bool pieMode = false;
    MetalRenderAPIImpl::PIEViewportTarget* pieTarget = nullptr;
    if (impl->activeSceneTarget >= 0) {
        auto it = impl->pieViewports.find(impl->activeSceneTarget);
        if (it != impl->pieViewports.end() && it->second.colorTexture) {
            pieMode = true;
            pieTarget = &it->second;
        }
    }

    int rtWidth, rtHeight;
    if (pieMode && pieTarget) {
        rtWidth = pieTarget->width;
        rtHeight = pieTarget->height;
    } else if (editorMode) {
        rtWidth = impl->viewportWidthRT;
        rtHeight = impl->viewportHeightRT;
    } else {
        rtWidth = impl->viewportWidth;
        rtHeight = impl->viewportHeight;
    }

    // End the current encoder — we need to create a parallel encoder
    if (impl->mainPassActive && impl->encoder) {
        [impl->encoder endEncoding];
        impl->encoder = nil;
        impl->mainPassActive = false;
    }

    // Build render pass descriptor with load actions (preserve shadow/clear content)
    MTLRenderPassDescriptor* passDesc = buildLoadPassDescriptor(impl.get());

    // Create parallel encoder
    id<MTLParallelRenderCommandEncoder> parallelEncoder =
        [impl->commandBuffer parallelRenderCommandEncoderWithDescriptor:passDesc];
    if (!parallelEncoder) {
        LOG_ENGINE_ERROR("[Metal] Failed to create parallel render encoder, falling back");
        // Reopen regular encoder and fall back
        impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
        impl->mainPassActive = (impl->encoder != nil);
        if (impl->encoder) replayCommandBuffer(cmds);
        return;
    }
    parallelEncoder.label = @"Parallel Replay Encoder";

    // Determine worker count
    uint32_t numWorkers = std::min(4u, static_cast<uint32_t>(
        (cmds.size() + METAL_PARALLEL_THRESHOLD - 1) / METAL_PARALLEL_THRESHOLD));
    numWorkers = std::max(numWorkers, 1u);
    size_t chunkSize = (cmds.size() + numWorkers - 1) / numWorkers;

    // Prepare shared state
    impl->updatePerFrameUBO();
    MetalGlobalUBO globalUBO = impl->cachedPerFrameUBO;

    // Create subordinate encoders (creation order = draw order)
    std::vector<id<MTLRenderCommandEncoder>> subordinates(numWorkers);
    for (uint32_t w = 0; w < numWorkers; w++) {
        subordinates[w] = [parallelEncoder renderCommandEncoder];
        subordinates[w].label = [NSString stringWithFormat:@"Parallel Worker %u", w];
    }

    // Launch workers
    std::vector<std::future<void>> futures;
    futures.reserve(numWorkers);

    for (uint32_t w = 0; w < numWorkers; w++) {
        size_t start = w * chunkSize;
        size_t end = std::min(start + chunkSize, cmds.size());

        futures.push_back(std::async(std::launch::async,
            [implPtr = impl.get(), enc = subordinates[w], &cmds, start, end, globalUBO, rtWidth, rtHeight]()
            {
                @autoreleasepool {
                    replayCommandRange(implPtr, enc, cmds, start, end, globalUBO, rtWidth, rtHeight);
                }
            }));
    }

    // Wait for all workers
    for (auto& f : futures) f.get();

    // End parallel encoder
    [parallelEncoder endEncoding];

    // Reopen a regular encoder for subsequent rendering (skybox, debug lines, UI)
    MTLRenderPassDescriptor* resumeDesc = buildLoadPassDescriptor(impl.get());
    impl->encoder = [impl->commandBuffer renderCommandEncoderWithDescriptor:resumeDesc];
    if (impl->encoder) {
        impl->encoder.label = @"Main Encoder (Post-Parallel)";
        impl->mainPassActive = true;

        // Restore viewport and default state
        MTLViewport viewport = {0, 0, (double)rtWidth, (double)rtHeight, 0, 1};
        [impl->encoder setViewport:viewport];
        [impl->encoder setDepthStencilState:impl->depthLessEqual];
        [impl->encoder setCullMode:MTLCullModeBack];
        [impl->encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    } else {
        impl->mainPassActive = false;
        LOG_ENGINE_ERROR("[Metal] Failed to reopen encoder after parallel replay");
    }

    // Reset bind tracking since encoder changed
    impl->lastBoundPipeline = nil;
    impl->lastBoundDepthStencil = nil;
    impl->lastBoundVertexBuffer = nil;
    impl->lastBoundTextureHandle = INVALID_TEXTURE;
    impl->lastCullMode = MTLCullModeBack;
    impl->shadowMapBound = false;
    impl->perFrameUBOReady = false;
}

// ============================================================================
// Debug Line Rendering
// ============================================================================

void MetalRenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || !impl->frameStarted) return;
    if (impl->inShadowPass) return;

    id<MTLRenderCommandEncoder> enc = impl->encoder;
    if (!enc) return;
    if (!impl->debugLinePipeline) return;

    // Set debug line pipeline and state
    [enc setRenderPipelineState:impl->debugLinePipeline];
    [enc setDepthStencilState:impl->depthLessEqual];
    [enc setCullMode:MTLCullModeNone];

    // Identity model matrix for debug lines
    glm::mat4 identity(1.0f);
    [enc setVertexBytes:&identity length:sizeof(glm::mat4) atIndex:2];

    // Upload vertex data (use temp buffer for large batches, setVertexBytes for small)
    size_t dataSize = vertex_count * sizeof(vertex);
    id<MTLBuffer> lineBuffer = nil;
    if (dataSize > 4096) {
        lineBuffer = [impl->device newBufferWithBytes:vertices
                                               length:dataSize
                                              options:MTLResourceStorageModeShared];
        lineBuffer.label = @"Debug Lines VB";
        [enc setVertexBuffer:lineBuffer offset:0 atIndex:0];
    }

    // Build per-frame UBO once
    impl->updatePerFrameUBO();
    MetalGlobalUBO ubo = impl->cachedPerFrameUBO;

    // Batch draws by color (normals encode color in debug vertices)
    size_t i = 0;
    while (i < vertex_count) {
        glm::vec3 color(vertices[i].nx, vertices[i].ny, vertices[i].nz);
        size_t batch_start = i;

        while (i < vertex_count &&
               vertices[i].nx == color.r &&
               vertices[i].ny == color.g &&
               vertices[i].nz == color.b) {
            i++;
        }

        ubo.color = color;
        ubo.useTexture = 0;
        [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
        [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];

        // Bind default texture (shader still samples it even in unlit mode)
        [enc setFragmentTexture:impl->defaultTexture atIndex:0];
        [enc setFragmentSamplerState:impl->defaultSampler atIndex:0];

        if (lineBuffer) {
            [enc drawPrimitives:MTLPrimitiveTypeLine
                    vertexStart:batch_start
                    vertexCount:i - batch_start];
        } else {
            size_t batchBytes = (i - batch_start) * sizeof(vertex);
            [enc setVertexBytes:vertices + batch_start
                         length:batchBytes
                        atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeLine
                    vertexStart:0
                    vertexCount:i - batch_start];
        }
    }

    // Force rebind on next mesh draw
    impl->lastBoundPipeline = nil;
    impl->lastBoundVertexBuffer = nil;
}

// ============================================================================
// Depth Prepass
// ============================================================================

void MetalRenderAPI::beginDepthPrepass()
{
    if (!impl->encoder) return;

    impl->inDepthPrepass = true;

    [impl->encoder setRenderPipelineState:impl->depthPrepassPipeline];
    impl->lastBoundPipeline = impl->depthPrepassPipeline;
    [impl->encoder setDepthStencilState:impl->depthLessEqual];
    [impl->encoder setCullMode:MTLCullModeBack];

    // Upload per-frame UBO (vertex shader needs view/proj matrices)
    impl->updatePerFrameUBO();
    MetalGlobalUBO ubo = impl->cachedPerFrameUBO;
    [impl->encoder setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
}

void MetalRenderAPI::endDepthPrepass()
{
    impl->inDepthPrepass = false;
    impl->lastBoundPipeline = nil; // force rebind
}

void MetalRenderAPI::renderMeshDepthOnly(const mesh& m)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
    if (!impl->encoder) return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<MetalRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    MetalMesh* metalMesh = dynamic_cast<MetalMesh*>(m.gpu_mesh);
    if (!metalMesh) return;
    id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)metalMesh->getVertexBuffer();
    if (!vertexBuffer) return;

    // Upload model + normal matrix (vertex shader expects ModelData at buffer 2)
    struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
    modelData.model = impl->currentModelMatrix;
    modelData.normalMatrix = glm::transpose(glm::inverse(impl->currentModelMatrix));
    [impl->encoder setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

    // Bind vertex buffer and draw
    [impl->encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    if (metalMesh->isIndexed()) {
        id<MTLBuffer> indexBuffer = (__bridge id<MTLBuffer>)metalMesh->getIndexBuffer();
        [impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                  indexCount:metalMesh->getIndexCount()
                                   indexType:MTLIndexTypeUInt32
                                 indexBuffer:indexBuffer
                           indexBufferOffset:0];
    } else {
        [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                          vertexStart:0
                          vertexCount:metalMesh->getVertexCount()];
    }
}

// ============================================================================
// GPU Synchronization
// ============================================================================

void MetalRenderAPI::waitForGPU()
{
    if (!impl->device) return;

    // Drain all in-flight frame slots
    for (int i = 0; i < MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT; i++) {
        dispatch_semaphore_wait(impl->frameSemaphore, DISPATCH_TIME_FOREVER);
    }
    // Restore semaphore state so rendering can continue
    for (int i = 0; i < MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT; i++) {
        dispatch_semaphore_signal(impl->frameSemaphore);
    }
}
