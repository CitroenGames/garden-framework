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

// ============================================================================
// SSAO Settings
// ============================================================================

void MetalRenderAPI::setSSAOEnabled(bool enabled) { impl->ssaoEnabled = enabled; }
bool MetalRenderAPI::isSSAOEnabled() const { return impl->ssaoEnabled; }
void MetalRenderAPI::setSSAORadius(float radius) { impl->ssaoRadius = radius; }
void MetalRenderAPI::setSSAOIntensity(float intensity) { impl->ssaoIntensity = intensity; }
