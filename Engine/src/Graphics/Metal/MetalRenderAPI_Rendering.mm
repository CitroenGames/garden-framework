#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include "MetalMesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Utils/Vertex.hpp"

#include "stb_image.h"

// ============================================================================
// Camera & Matrix Stack
// ============================================================================

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

    // Get cached sampler
    MetalSamplerKey samplerKey{
        MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear,
        (mipLevels > 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped,
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeRepeat, 16,
        MTLCompareFunctionNever
    };
    id<MTLSamplerState> sampler = impl->samplerCache.getOrCreate(samplerKey);

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

    MetalSamplerKey samplerKey{
        MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear,
        (mip_count > 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped,
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeRepeat, 16,
        MTLCompareFunctionNever
    };
    id<MTLSamplerState> sampler = impl->samplerCache.getOrCreate(samplerKey);

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
    if (texture != INVALID_TEXTURE && impl->textures.count(texture)) {
        MetalTexture tex = impl->textures[texture]; // copy by value (ARC retains)
        impl->textures.erase(texture);
        impl->deletionQueue.push([tex]() {
            // ARC releases the id<MTLTexture> and id<MTLSamplerState> when the
            // lambda (and its captured MetalTexture copy) is destroyed
        }, MetalRenderAPIImpl::MAX_FRAMES_IN_FLIGHT);
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

    // Set model + normal matrix
    struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
    modelData.model = impl->currentModelMatrix;
    modelData.normalMatrix = glm::transpose(glm::inverse(impl->currentModelMatrix));
    [impl->encoder setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

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

    // Model + normal matrix per draw
    struct { glm::mat4 model; glm::mat4 normalMatrix; } modelData;
    modelData.model = impl->currentModelMatrix;
    modelData.normalMatrix = glm::transpose(glm::inverse(impl->currentModelMatrix));
    [impl->encoder setVertexBytes:&modelData length:sizeof(modelData) atIndex:2];

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
// FXAA Settings
// ============================================================================

void MetalRenderAPI::setFXAAEnabled(bool enabled)
{
    impl->fxaaEnabled = enabled;
}

bool MetalRenderAPI::isFXAAEnabled() const
{
    return impl->fxaaEnabled;
}
