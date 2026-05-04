#include "MetalRenderAPI.hpp"
#include "MetalRenderAPI_Impl.hpp"
#include <random>
#include <cmath>

// ============================================================================
// SSAO Kernel Generation
// ============================================================================

static void generateSSAOKernel(glm::vec4 kernel[16])
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distNeg11(-1.0f, 1.0f);

    for (int i = 0; i < 16; i++) {
        glm::vec3 sample(distNeg11(rng), distNeg11(rng), dist01(rng));
        sample = glm::normalize(sample);
        sample *= dist01(rng);

        float scale = static_cast<float>(i) / 16.0f;
        scale = 0.1f + scale * scale * (1.0f - 0.1f);
        sample *= scale;

        kernel[i] = glm::vec4(sample, 0.0f);
    }
}

// ============================================================================
// SSAO Metal Uniform Structures (must match Metal shader)
// ============================================================================

struct MetalSSAOUniforms {
    glm::mat4 projection;
    glm::mat4 invProjection;
    glm::vec4 samples[16];
    glm::vec2 screenSize;
    glm::vec2 noiseScale;
    float radius;
    float bias;
    float power;
    float _pad;
};

struct MetalSSAOBlurUniforms {
    glm::vec2 texelSize;
    glm::vec2 blurDir;
    float depthThreshold;
};

void MetalRenderAPIImpl::createSSAOResources(int w, int h)
{
    ssaoInitialized = false;
    ssaoWidth = w;
    ssaoHeight = h;

    if (w <= 0 || h <= 0 || !device || !ssaoPipeline || !ssaoBlurPipeline) {
        return;
    }

    generateSSAOKernel(ssaoKernel);

    const int halfW = std::max(1, w / 2);
    const int halfH = std::max(1, h / 2);

    MTLTextureDescriptor* aoDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:halfW
                                                                                     height:halfH
                                                                                  mipmapped:NO];
    aoDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    aoDesc.storageMode = MTLStorageModePrivate;

    ssaoRawTexture = [device newTextureWithDescriptor:aoDesc];
    ssaoRawTexture.label = @"SSAO Raw";
    ssaoBlurTempTexture = [device newTextureWithDescriptor:aoDesc];
    ssaoBlurTempTexture.label = @"SSAO Blur Temp";
    ssaoBlurredTexture = [device newTextureWithDescriptor:aoDesc];
    ssaoBlurredTexture.label = @"SSAO Blurred";

    if (!ssaoNoiseTexture) {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        float noiseData[16 * 4];
        for (int i = 0; i < 16; i++) {
            float x = dist(rng);
            float y = dist(rng);
            float len = std::sqrt(x * x + y * y);
            if (len > 0.0001f) {
                x /= len;
                y /= len;
            }
            noiseData[i * 4 + 0] = x;
            noiseData[i * 4 + 1] = y;
            noiseData[i * 4 + 2] = 0.0f;
            noiseData[i * 4 + 3] = 0.0f;
        }

        MTLTextureDescriptor* noiseDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                             width:4
                                                                                            height:4
                                                                                         mipmapped:NO];
        noiseDesc.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
        noiseDesc.storageMode = MTLStorageModeManaged;
#else
        noiseDesc.storageMode = MTLStorageModeShared;
#endif
        ssaoNoiseTexture = [device newTextureWithDescriptor:noiseDesc];
        ssaoNoiseTexture.label = @"SSAO Noise";
        [ssaoNoiseTexture replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
                            mipmapLevel:0
                              withBytes:noiseData
                            bytesPerRow:sizeof(float) * 4 * 4];
    }

    if (!ssaoDepthSampler) {
        MTLSamplerDescriptor* depthDesc = [[MTLSamplerDescriptor alloc] init];
        depthDesc.minFilter = MTLSamplerMinMagFilterNearest;
        depthDesc.magFilter = MTLSamplerMinMagFilterNearest;
        depthDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        depthDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        ssaoDepthSampler = [device newSamplerStateWithDescriptor:depthDesc];
    }

    if (!ssaoNoiseSampler) {
        MTLSamplerDescriptor* noiseDesc = [[MTLSamplerDescriptor alloc] init];
        noiseDesc.minFilter = MTLSamplerMinMagFilterNearest;
        noiseDesc.magFilter = MTLSamplerMinMagFilterNearest;
        noiseDesc.sAddressMode = MTLSamplerAddressModeRepeat;
        noiseDesc.tAddressMode = MTLSamplerAddressModeRepeat;
        ssaoNoiseSampler = [device newSamplerStateWithDescriptor:noiseDesc];
    }

    ssaoInitialized = ssaoRawTexture && ssaoBlurTempTexture && ssaoBlurredTexture
        && ssaoNoiseTexture && ssaoDepthSampler && ssaoNoiseSampler;

    if (ssaoInitialized) {
        LOG_ENGINE_INFO("[Metal] SSAO resources created ({}x{})", halfW, halfH);
    }
}

id<MTLTexture> MetalRenderAPIImpl::runSSAOPasses(id<MTLTexture> depthTexture, int w, int h)
{
    if (!ssaoEnabled || !depthTexture || !commandBuffer || !fxaaVertexBuffer
        || !ssaoPipeline || !ssaoBlurPipeline) {
        return nil;
    }

    if (!ssaoInitialized || ssaoWidth != w || ssaoHeight != h) {
        createSSAOResources(w, h);
    }

    if (!ssaoInitialized || !ssaoRawTexture || !ssaoBlurTempTexture || !ssaoBlurredTexture) {
        return nil;
    }

    const int halfW = std::max(1, w / 2);
    const int halfH = std::max(1, h / 2);

    MetalSSAOUniforms ssaoUniforms{};
    ssaoUniforms.projection = projectionMatrix;
    ssaoUniforms.invProjection = glm::inverse(projectionMatrix);
    for (int i = 0; i < 16; i++) {
        ssaoUniforms.samples[i] = ssaoKernel[i];
    }
    ssaoUniforms.screenSize = glm::vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    ssaoUniforms.noiseScale = ssaoUniforms.screenSize / 4.0f;
    ssaoUniforms.radius = ssaoRadius;
    ssaoUniforms.bias = ssaoBias;
    ssaoUniforms.power = ssaoIntensity;

    MTLViewport ssaoViewport = {0, 0, static_cast<double>(halfW), static_cast<double>(halfH), 0, 1};

    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = ssaoRawTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (!enc) return nil;
        enc.label = @"SSAO Compute";
        [enc setRenderPipelineState:ssaoPipeline];
        [enc setViewport:ssaoViewport];
        [enc setFragmentTexture:depthTexture atIndex:0];
        [enc setFragmentTexture:ssaoNoiseTexture atIndex:1];
        [enc setFragmentSamplerState:ssaoDepthSampler atIndex:0];
        [enc setFragmentSamplerState:ssaoNoiseSampler atIndex:1];
        [enc setFragmentBytes:&ssaoUniforms length:sizeof(ssaoUniforms) atIndex:0];
        [enc setVertexBuffer:fxaaVertexBuffer offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc endEncoding];
    }

    {
        MetalSSAOBlurUniforms blurH{};
        blurH.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurH.blurDir = glm::vec2(1.0f, 0.0f);
        blurH.depthThreshold = 0.001f;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = ssaoBlurTempTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (!enc) return nil;
        enc.label = @"SSAO Blur H";
        [enc setRenderPipelineState:ssaoBlurPipeline];
        [enc setViewport:ssaoViewport];
        [enc setFragmentTexture:ssaoRawTexture atIndex:0];
        [enc setFragmentTexture:depthTexture atIndex:1];
        [enc setFragmentSamplerState:defaultSampler atIndex:0];
        [enc setFragmentSamplerState:ssaoDepthSampler atIndex:1];
        [enc setFragmentBytes:&blurH length:sizeof(blurH) atIndex:0];
        [enc setVertexBuffer:fxaaVertexBuffer offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc endEncoding];
    }

    {
        MetalSSAOBlurUniforms blurV{};
        blurV.texelSize = glm::vec2(1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
        blurV.blurDir = glm::vec2(0.0f, 1.0f);
        blurV.depthThreshold = 0.001f;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = ssaoBlurredTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (!enc) return nil;
        enc.label = @"SSAO Blur V";
        [enc setRenderPipelineState:ssaoBlurPipeline];
        [enc setViewport:ssaoViewport];
        [enc setFragmentTexture:ssaoBlurTempTexture atIndex:0];
        [enc setFragmentTexture:depthTexture atIndex:1];
        [enc setFragmentSamplerState:defaultSampler atIndex:0];
        [enc setFragmentSamplerState:ssaoDepthSampler atIndex:1];
        [enc setFragmentBytes:&blurV length:sizeof(blurV) atIndex:0];
        [enc setVertexBuffer:fxaaVertexBuffer offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc endEncoding];
    }

    return ssaoBlurredTexture;
}

// ============================================================================
// SSAO Settings
// ============================================================================

void MetalRenderAPI::setSSAOEnabled(bool enabled) { impl->ssaoEnabled = enabled; }
bool MetalRenderAPI::isSSAOEnabled() const { return impl->ssaoEnabled; }
void MetalRenderAPI::setSSAORadius(float radius) { impl->ssaoRadius = radius; }
void MetalRenderAPI::setSSAOIntensity(float intensity) { impl->ssaoIntensity = intensity; }
