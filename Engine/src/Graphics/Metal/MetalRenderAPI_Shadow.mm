#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include "Components/camera.hpp"

#include <limits>

// ============================================================================
// MetalRenderAPIImpl helper definitions (shadow-related)
// ============================================================================

void MetalRenderAPIImpl::createShadowResources()
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

void MetalRenderAPIImpl::calculateCascadeSplits(float nearPlane, float farPlane)
{
    cascadeSplitDistances[0] = nearPlane;
    for (int i = 1; i <= NUM_CASCADES; i++) {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log_split = nearPlane * std::pow(farPlane / nearPlane, p);
        float linear = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log_split + (1.0f - cascadeSplitLambda) * linear;
    }
}

std::array<glm::vec3, 8> MetalRenderAPIImpl::getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
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

glm::mat4 MetalRenderAPIImpl::getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
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

void MetalRenderAPIImpl::recreateShadowResources(uint32_t size)
{
    shadowMapSize = size;
    shadowMapArray = nil;
    shadowSampler = nil;
    createShadowResources();
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
// Shadow Quality Settings
// ============================================================================

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
