// SSAO noise tiling tests: replicates the SSAO rotation, kernel occlusion
// and bilateral blur math from assets/shaders/slang/ssao.slang and
// ssao_blur.slang together with the kernel/noise generation and per-frame
// constants used by the Vulkan and D3D12 backends, on a flat ground plane.
// Run: SSAONoiseTests.exe - exit code 0 means all tests passed.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

// Backend constants (VulkanRenderAPI.hpp / D3D12RenderAPI.hpp defaults,
// VulkanRenderAPI_Frame.cpp per-frame UBO fill).
static constexpr int kFullW = 1920;
static constexpr int kFullH = 1080;
static constexpr int kHalfW = kFullW / 2;
static constexpr int kHalfH = kFullH / 2;
static constexpr int kKernelSize = 16;
static constexpr float kRadius = 0.5f;
static constexpr float kBias = 0.025f;
static constexpr float kPower = 1.5f;
static constexpr float kBlurDepthThreshold = 0.005f;
static constexpr float kFovDegrees = 60.0f;

// Correct-behaviour limits for a uniformly occluded flat surface: the AO
// value must not depend on which of the 16 tiled rotation phases a pixel
// falls in, tile borders must not stand out from interior pixel steps, and
// the AO residual must not repeat more strongly at the 4-texel tile period
// than at a 1-texel lag. AO is stored R8_UNORM (1/255 steps), so a 0.01
// phase spread is already several quantization steps.
static constexpr double kMaxPhaseSpread = 0.01;
static constexpr double kMaxBorderRatio = 1.10;
static constexpr double kMaxLag4Excess = 0.10;

// Hemisphere kernel, copied from generateSSAOKernel() in
// VulkanRenderAPI_SSAO.cpp / D3D12RenderAPI_SSAO.cpp (seed 42).
static std::array<glm::vec4, kKernelSize> generateKernel()
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distNeg11(-1.0f, 1.0f);

    std::array<glm::vec4, kKernelSize> kernel{};
    for (int i = 0; i < kKernelSize; i++)
    {
        glm::vec3 sample(distNeg11(rng), distNeg11(rng), dist01(rng));
        sample = glm::normalize(sample);
        sample *= dist01(rng);

        float scale = static_cast<float>(i) / 16.0f;
        scale = 0.1f + scale * scale * (1.0f - 0.1f);
        sample *= scale;

        kernel[i] = glm::vec4(sample, 0.0f);
    }
    return kernel;
}

// 4x4 rotation noise, copied from createSSAOResources() in
// VulkanRenderAPI_SSAO.cpp / D3D12RenderAPI_SSAO.cpp (seed 12345).
// Texel (x, y) lives at index y * 4 + x, z and w are zero.
static std::array<glm::vec3, 16> generateNoise()
{
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::array<glm::vec3, 16> noise{};
    for (int i = 0; i < 16; i++)
    {
        float x = dist(rng), y = dist(rng);
        float len = std::sqrt(x * x + y * y);
        if (len > 0.0001f) { x /= len; y /= len; }
        noise[i] = glm::vec3(x, y, 0.0f);
    }
    return noise;
}

// Projection used by the backends: glm::perspectiveRH_ZO with the Vulkan
// Y flip applied to element [1][1] (VulkanRenderAPI_Viewport.cpp).
static glm::mat4 makeProjection()
{
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(kFovDegrees),
                                           static_cast<float>(kFullW) / static_cast<float>(kFullH),
                                           0.1f, 1000.0f);
    proj[1][1] *= -1.0f;
    return proj;
}

// ssao.slang ReconstructViewPos (Vulkan branch).
static glm::vec3 reconstructViewPos(const glm::vec2& uv, float depth, const glm::mat4& invProj)
{
    glm::vec2 ndc = uv * 2.0f - 1.0f;
    glm::vec4 clipPos(ndc.x, -ndc.y, depth, 1.0f);
    glm::vec4 viewPos = invProj * clipPos;
    return glm::vec3(viewPos) / viewPos.w;
}

static float fracf(float v) { return v - std::floor(v); }

// NEAREST + CLAMP_TO_EDGE fetch from the full-res depth buffer, matching
// depthTexture.Sample(depthSampler, uv) with the backend depth sampler.
static float sampleDepthNearest(const std::vector<float>& depth, const glm::vec2& uv)
{
    int x = static_cast<int>(std::floor(uv.x * static_cast<float>(kFullW)));
    int y = static_cast<int>(std::floor(uv.y * static_cast<float>(kFullH)));
    x = glm::clamp(x, 0, kFullW - 1);
    y = glm::clamp(y, 0, kFullH - 1);
    return depth[static_cast<size_t>(y) * kFullW + x];
}

// NEAREST fetch from a half-res buffer (ssao_blur.slang ssaoInput sampling).
static float sampleHalfNearest(const std::vector<float>& buf, const glm::vec2& uv)
{
    int x = static_cast<int>(std::floor(uv.x * static_cast<float>(kHalfW)));
    int y = static_cast<int>(std::floor(uv.y * static_cast<float>(kHalfH)));
    x = glm::clamp(x, 0, kHalfW - 1);
    y = glm::clamp(y, 0, kHalfH - 1);
    return buf[static_cast<size_t>(y) * kHalfW + x];
}

// SnapDepthUV in ssao.slang: centre of the full-res depth texel under uv.
static glm::vec2 snapDepthUV(const glm::vec2& uv)
{
    glm::vec2 depthSize{static_cast<float>(kFullW), static_cast<float>(kFullH)};
    return (glm::floor(uv * depthSize) + 0.5f) / depthSize;
}

// ssao.slang ReconstructNormal (Vulkan branch: normalize(cross(dx, dy))).
static glm::vec3 reconstructNormal(const std::vector<float>& depth, const glm::vec2& uv,
                                   const glm::vec2& texelSize, const glm::vec3& viewPos,
                                   const glm::mat4& invProj)
{
    glm::vec2 uvRight = snapDepthUV(uv + glm::vec2(texelSize.x, 0.0f));
    glm::vec2 uvLeft = snapDepthUV(uv - glm::vec2(texelSize.x, 0.0f));
    glm::vec2 uvUp = snapDepthUV(uv + glm::vec2(0.0f, texelSize.y));
    glm::vec2 uvDown = snapDepthUV(uv - glm::vec2(0.0f, texelSize.y));

    float depthRight = sampleDepthNearest(depth, uvRight);
    float depthLeft = sampleDepthNearest(depth, uvLeft);
    float depthUp = sampleDepthNearest(depth, uvUp);
    float depthDown = sampleDepthNearest(depth, uvDown);

    glm::vec3 posRight = reconstructViewPos(uvRight, depthRight, invProj);
    glm::vec3 posLeft = reconstructViewPos(uvLeft, depthLeft, invProj);
    glm::vec3 posUp = reconstructViewPos(uvUp, depthUp, invProj);
    glm::vec3 posDown = reconstructViewPos(uvDown, depthDown, invProj);

    glm::vec3 dx = (std::abs(posRight.z - viewPos.z) < std::abs(viewPos.z - posLeft.z))
                       ? (posRight - viewPos) : (viewPos - posLeft);
    glm::vec3 dy = (std::abs(posUp.z - viewPos.z) < std::abs(viewPos.z - posDown.z))
                       ? (posUp - viewPos) : (viewPos - posDown);
    return glm::normalize(glm::cross(dx, dy));
}

static float smoothstepf(float edge0, float edge1, float x)
{
    float t = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Scene
{
    std::vector<float> depth;      // full-res, Vulkan [0,1] depth, 1.0 = sky
    std::vector<uint8_t> ground;   // full-res, 1 where the ray hit the ground plane
};

// Renders the flat ground plane (world y = 0) into a full-res depth buffer
// for a camera at `eye` looking at `target`, using the same projection and
// view-position reconstruction as the shader so rendered and reconstructed
// positions agree.
static Scene renderGround(const glm::vec3& eye, const glm::vec3& target,
                          const glm::mat4& proj, const glm::mat4& invProj)
{
    Scene scene;
    scene.depth.assign(static_cast<size_t>(kFullW) * kFullH, 1.0f);
    scene.ground.assign(static_cast<size_t>(kFullW) * kFullH, 0);

    glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat3 invRot = glm::transpose(glm::mat3(view));

    for (int y = 0; y < kFullH; y++)
    {
        for (int x = 0; x < kFullW; x++)
        {
            glm::vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(kFullW),
                         (static_cast<float>(y) + 0.5f) / static_cast<float>(kFullH));
            glm::vec3 nearPos = reconstructViewPos(uv, 0.0f, invProj);
            glm::vec3 farPos = reconstructViewPos(uv, 1.0f, invProj);
            glm::vec3 dirView = glm::normalize(farPos - nearPos);
            glm::vec3 dirWorld = invRot * dirView;

            size_t idx = static_cast<size_t>(y) * kFullW + x;
            if (dirWorld.y >= -1e-6f)
                continue; // ray never reaches the ground: sky

            float t = -eye.y / dirWorld.y;
            glm::vec3 hitWorld = eye + dirWorld * t;
            glm::vec3 hitView = glm::vec3(view * glm::vec4(hitWorld, 1.0f));
            glm::vec4 clip = proj * glm::vec4(hitView, 1.0f);
            scene.depth[idx] = clip.z / clip.w;
            scene.ground[idx] = 1;
        }
    }
    return scene;
}

// Per-pixel rotation vector for the tiled path: ssao.slang samples
// noiseTexture at frac(uv * noiseScale) with a NEAREST/REPEAT sampler and
// noiseScale = halfRes / 4, so the fetched texel repeats every 4 half-res
// texels. The untiled control instead derives a fresh rotation per pixel.
static glm::vec3 rotationForPixel(int px, int py, bool tiled,
                                  const std::array<glm::vec3, 16>& noise)
{
    if (tiled)
    {
        glm::vec2 uv((static_cast<float>(px) + 0.5f) / static_cast<float>(kHalfW),
                     (static_cast<float>(py) + 0.5f) / static_cast<float>(kHalfH));
        glm::vec2 noiseScale(static_cast<float>(kHalfW) / 4.0f, static_cast<float>(kHalfH) / 4.0f);
        glm::vec2 tiledUv(fracf(uv.x * noiseScale.x), fracf(uv.y * noiseScale.y));
        int tx = glm::clamp(static_cast<int>(std::floor(tiledUv.x * 4.0f)), 0, 3);
        int ty = glm::clamp(static_cast<int>(std::floor(tiledUv.y * 4.0f)), 0, 3);
        return noise[static_cast<size_t>(ty) * 4 + tx];
    }

    uint32_t h = static_cast<uint32_t>(px) * 1973u + static_cast<uint32_t>(py) * 9277u + 26699u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    float angle = static_cast<float>(h % 36000u) / 36000.0f * 6.28318530718f;
    return glm::vec3(std::cos(angle), std::sin(angle), 0.0f);
}

// ssao.slang fragmentMain at half resolution over the given depth buffer.
// Pixels whose centre depth is sky (>= 0.9999) produce 1.0, as in the shader.
static std::vector<float> runSSAO(const Scene& scene, bool tiled,
                                  const std::array<glm::vec4, kKernelSize>& kernel,
                                  const std::array<glm::vec3, 16>& noise,
                                  const glm::mat4& proj, const glm::mat4& invProj,
                                  int& nanCount)
{
    std::vector<float> ao(static_cast<size_t>(kHalfW) * kHalfH, 1.0f);
    nanCount = 0;
    const glm::vec2 depthTexelSize(1.0f / (static_cast<float>(kHalfW) * 2.0f),
                                   1.0f / (static_cast<float>(kHalfH) * 2.0f));

    for (int py = 0; py < kHalfH; py++)
    {
        for (int px = 0; px < kHalfW; px++)
        {
            glm::vec2 uv((static_cast<float>(px) + 0.5f) / static_cast<float>(kHalfW),
                         (static_cast<float>(py) + 0.5f) / static_cast<float>(kHalfH));
            glm::vec2 depthUv = snapDepthUV(uv);
            float depth = sampleDepthNearest(scene.depth, depthUv);
            size_t idx = static_cast<size_t>(py) * kHalfW + px;
            if (depth >= 0.9999f)
            {
                ao[idx] = 1.0f;
                continue;
            }

            glm::vec3 fragPos = reconstructViewPos(depthUv, depth, invProj);
            glm::vec3 normal = reconstructNormal(scene.depth, depthUv, depthTexelSize, fragPos, invProj);

            glm::vec3 randomVec = rotationForPixel(px, py, tiled, noise);
            glm::vec3 tangent = glm::normalize(randomVec - normal * glm::dot(randomVec, normal));
            glm::vec3 bitangent = glm::cross(normal, tangent);

            float occlusion = 0.0f;
            for (int i = 0; i < kKernelSize; i++)
            {
                glm::vec3 sampleDir = tangent * kernel[i].x + bitangent * kernel[i].y + normal * kernel[i].z;
                glm::vec3 samplePos = fragPos + sampleDir * kRadius;

                glm::vec4 offset = proj * glm::vec4(samplePos, 1.0f);
                offset.x /= offset.w;
                offset.y /= offset.w;
                offset.z /= offset.w;
                glm::vec2 sampleUV(offset.x * 0.5f + 0.5f, offset.y * 0.5f + 0.5f);
                sampleUV.y = 1.0f - sampleUV.y;

                float sampleDepth = sampleDepthNearest(scene.depth, sampleUV);
                glm::vec3 sampleViewPos = reconstructViewPos(sampleUV, sampleDepth, invProj);

                float rangeCheck = smoothstepf(0.0f, 1.0f,
                    kRadius / glm::max(std::abs(fragPos.z - sampleViewPos.z), 0.0001f));
                occlusion += (sampleViewPos.z >= samplePos.z + kBias ? 1.0f : 0.0f) * rangeCheck;
            }

            occlusion = 1.0f - (occlusion / static_cast<float>(kKernelSize));
            occlusion = std::pow(glm::clamp(occlusion, 0.0f, 1.0f), kPower);
            if (std::isnan(occlusion))
            {
                nanCount++;
                occlusion = 1.0f;
            }
            ao[idx] = occlusion;
        }
    }
    return ao;
}

// ssao_blur.slang: one separable 9-tap bilateral pass. Depth comes from the
// full-res depth buffer sampled at the half-res uv, weights are the shader
// Gaussian table gated by step(abs(depthDelta), depthThreshold).
static std::vector<float> runBlurPass(const std::vector<float>& input, const Scene& scene,
                                      const glm::vec2& blurDir)
{
    static constexpr float gaussWeights[5] = {0.227027f, 0.194596f, 0.121622f, 0.054054f, 0.016216f};
    const glm::vec2 texelSize(1.0f / static_cast<float>(kHalfW), 1.0f / static_cast<float>(kHalfH));
    std::vector<float> out(input.size(), 1.0f);

    for (int py = 0; py < kHalfH; py++)
    {
        for (int px = 0; px < kHalfW; px++)
        {
            glm::vec2 uv((static_cast<float>(px) + 0.5f) / static_cast<float>(kHalfW),
                         (static_cast<float>(py) + 0.5f) / static_cast<float>(kHalfH));
            float centerAO = sampleHalfNearest(input, uv);
            float centerDepth = sampleDepthNearest(scene.depth, uv);

            float result = centerAO * gaussWeights[0];
            float totalWeight = gaussWeights[0];
            for (int i = 1; i < 5; i++)
            {
                glm::vec2 offset = blurDir * texelSize * static_cast<float>(i);

                glm::vec2 uvPos = uv + offset;
                float weightPos = gaussWeights[i] *
                    (std::abs(sampleDepthNearest(scene.depth, uvPos) - centerDepth) <= kBlurDepthThreshold ? 1.0f : 0.0f);
                result += sampleHalfNearest(input, uvPos) * weightPos;
                totalWeight += weightPos;

                glm::vec2 uvNeg = uv - offset;
                float weightNeg = gaussWeights[i] *
                    (std::abs(sampleDepthNearest(scene.depth, uvNeg) - centerDepth) <= kBlurDepthThreshold ? 1.0f : 0.0f);
                result += sampleHalfNearest(input, uvNeg) * weightNeg;
                totalWeight += weightNeg;
            }
            out[static_cast<size_t>(py) * kHalfW + px] = result / glm::max(totalWeight, 0.0001f);
        }
    }
    return out;
}

struct Metrics
{
    double meanAO = 0.0;
    double phaseSpread = 0.0;   // max - min of the 16 tile-phase mean AO values
    double phaseStd = 0.0;      // std-dev of the 16 tile-phase means
    double withinStd = 0.0;     // std-dev of AO around its own phase mean
    double borderStep = 0.0;    // mean |AO step| across tile borders (phase 3 -> 0)
    double interiorStep = 0.0;  // mean |AO step| across interior pixel borders
    double borderRatio = 0.0;   // borderStep / interiorStep
    double lag1Corr = 0.0;      // residual autocorrelation at lag (1,0) and (0,1)
    double lag4Corr = 0.0;      // residual autocorrelation at the tile period (4,0)/(0,4)
    long groundPixels = 0;
};

// Statistics over ground pixels only. Each of the 16 rotation phases
// (px mod 4, py mod 4) samples the whole image uniformly, so a difference
// between phase means cannot come from the smooth occlusion gradient of
// the flat plane: it is the signature of the tiled rotation.
static Metrics measure(const std::vector<float>& ao, const Scene& scene)
{
    std::vector<uint8_t> groundHalf(static_cast<size_t>(kHalfW) * kHalfH, 0);
    for (int py = 0; py < kHalfH; py++)
        for (int px = 0; px < kHalfW; px++)
        {
            glm::vec2 uv((static_cast<float>(px) + 0.5f) / static_cast<float>(kHalfW),
                         (static_cast<float>(py) + 0.5f) / static_cast<float>(kHalfH));
            int fx = glm::clamp(static_cast<int>(std::floor(uv.x * kFullW)), 0, kFullW - 1);
            int fy = glm::clamp(static_cast<int>(std::floor(uv.y * kFullH)), 0, kFullH - 1);
            groundHalf[static_cast<size_t>(py) * kHalfW + px] = scene.ground[static_cast<size_t>(fy) * kFullW + fx];
        }

    Metrics m;
    double phaseSum[16] = {};
    long phaseCount[16] = {};
    double total = 0.0;
    for (int py = 0; py < kHalfH; py++)
        for (int px = 0; px < kHalfW; px++)
        {
            size_t idx = static_cast<size_t>(py) * kHalfW + px;
            if (!groundHalf[idx]) continue;
            int phase = (py % 4) * 4 + (px % 4);
            phaseSum[phase] += ao[idx];
            phaseCount[phase]++;
            total += ao[idx];
            m.groundPixels++;
        }

    double phaseMean[16] = {};
    double lo = 1e30, hi = -1e30, meanOfMeans = 0.0;
    for (int i = 0; i < 16; i++)
    {
        phaseMean[i] = phaseCount[i] ? phaseSum[i] / static_cast<double>(phaseCount[i]) : 0.0;
        lo = glm::min(lo, phaseMean[i]);
        hi = glm::max(hi, phaseMean[i]);
        meanOfMeans += phaseMean[i] / 16.0;
    }
    m.meanAO = m.groundPixels ? total / static_cast<double>(m.groundPixels) : 0.0;
    m.phaseSpread = hi - lo;
    double varMeans = 0.0;
    for (int i = 0; i < 16; i++) varMeans += (phaseMean[i] - meanOfMeans) * (phaseMean[i] - meanOfMeans);
    m.phaseStd = std::sqrt(varMeans / 16.0);

    std::vector<double> residual(ao.size(), 0.0);
    double varWithin = 0.0;
    for (int py = 0; py < kHalfH; py++)
        for (int px = 0; px < kHalfW; px++)
        {
            size_t idx = static_cast<size_t>(py) * kHalfW + px;
            if (!groundHalf[idx]) continue;
            double r = static_cast<double>(ao[idx]) - phaseMean[(py % 4) * 4 + (px % 4)];
            residual[idx] = r;
            varWithin += r * r;
        }
    m.withinStd = m.groundPixels ? std::sqrt(varWithin / static_cast<double>(m.groundPixels)) : 0.0;

    double borderSum = 0.0, interiorSum = 0.0;
    long borderN = 0, interiorN = 0;
    for (int py = 0; py < kHalfH; py++)
        for (int px = 1; px < kHalfW; px++)
        {
            size_t a = static_cast<size_t>(py) * kHalfW + px - 1;
            size_t b = static_cast<size_t>(py) * kHalfW + px;
            if (!groundHalf[a] || !groundHalf[b]) continue;
            double step = std::abs(static_cast<double>(ao[b]) - static_cast<double>(ao[a]));
            if (px % 4 == 0) { borderSum += step; borderN++; }
            else { interiorSum += step; interiorN++; }
        }
    for (int py = 1; py < kHalfH; py++)
        for (int px = 0; px < kHalfW; px++)
        {
            size_t a = static_cast<size_t>(py - 1) * kHalfW + px;
            size_t b = static_cast<size_t>(py) * kHalfW + px;
            if (!groundHalf[a] || !groundHalf[b]) continue;
            double step = std::abs(static_cast<double>(ao[b]) - static_cast<double>(ao[a]));
            if (py % 4 == 0) { borderSum += step; borderN++; }
            else { interiorSum += step; interiorN++; }
        }
    m.borderStep = borderN ? borderSum / static_cast<double>(borderN) : 0.0;
    m.interiorStep = interiorN ? interiorSum / static_cast<double>(interiorN) : 0.0;
    m.borderRatio = m.interiorStep > 1e-12 ? m.borderStep / m.interiorStep : 0.0;

    auto lagCorr = [&](int dx, int dy) {
        double num = 0.0, den = 0.0;
        long n = 0;
        for (int py = 0; py + dy < kHalfH; py++)
            for (int px = 0; px + dx < kHalfW; px++)
            {
                size_t a = static_cast<size_t>(py) * kHalfW + px;
                size_t b = static_cast<size_t>(py + dy) * kHalfW + (px + dx);
                if (!groundHalf[a] || !groundHalf[b]) continue;
                num += residual[a] * residual[b];
                den += residual[a] * residual[a];
                n++;
            }
        return (n && den > 1e-18) ? num / den : 0.0;
    };
    m.lag1Corr = 0.5 * (lagCorr(1, 0) + lagCorr(0, 1));
    m.lag4Corr = 0.5 * (lagCorr(4, 0) + lagCorr(0, 4));
    return m;
}

static void printMetrics(const char* label, const Metrics& m)
{
    std::cout << "  " << label
              << " meanAO=" << m.meanAO
              << " phaseSpread=" << m.phaseSpread
              << " phaseStd=" << m.phaseStd
              << " withinStd=" << m.withinStd
              << " borderStep=" << m.borderStep
              << " interiorStep=" << m.interiorStep
              << " borderRatio=" << m.borderRatio
              << " lag1Corr=" << m.lag1Corr
              << " lag4Corr=" << m.lag4Corr
              << " groundPx=" << m.groundPixels << std::endl;
}

static bool checkView(const std::string& viewName, const Scene& scene,
                      const std::array<glm::vec4, kKernelSize>& kernel,
                      const std::array<glm::vec3, 16>& noise,
                      const glm::mat4& proj, const glm::mat4& invProj)
{
    bool ok = true;
    int nanRaw = 0;
    std::vector<float> rawTiled = runSSAO(scene, true, kernel, noise, proj, invProj, nanRaw);
    std::vector<float> blurH = runBlurPass(rawTiled, scene, glm::vec2(1.0f, 0.0f));
    std::vector<float> blurV = runBlurPass(blurH, scene, glm::vec2(0.0f, 1.0f));

    int nanControl = 0;
    std::vector<float> rawControl = runSSAO(scene, false, kernel, noise, proj, invProj, nanControl);
    std::vector<float> controlH = runBlurPass(rawControl, scene, glm::vec2(1.0f, 0.0f));
    std::vector<float> controlV = runBlurPass(controlH, scene, glm::vec2(0.0f, 1.0f));

    Metrics raw = measure(rawTiled, scene);
    Metrics blurred = measure(blurV, scene);
    Metrics controlRaw = measure(rawControl, scene);
    Metrics controlBlurred = measure(controlV, scene);

    std::cout << "view " << viewName << " (NaN raw=" << nanRaw << " control=" << nanControl << ")" << std::endl;
    printMetrics("tiled raw     ", raw);
    printMetrics("tiled blurred ", blurred);
    printMetrics("control raw   ", controlRaw);
    printMetrics("control blurred", controlBlurred);

    std::string name = viewName + ": raw AO phase spread within limit";
    if (raw.phaseSpread > kMaxPhaseSpread)
        ok &= fail(name, "phaseSpread=" + std::to_string(raw.phaseSpread) + " > " + std::to_string(kMaxPhaseSpread));
    else
        ok &= pass(name);

    name = viewName + ": blurred AO phase spread within limit";
    if (blurred.phaseSpread > kMaxPhaseSpread)
        ok &= fail(name, "phaseSpread=" + std::to_string(blurred.phaseSpread) + " > " + std::to_string(kMaxPhaseSpread));
    else
        ok &= pass(name);

    name = viewName + ": blurred tile-border step within limit";
    if (blurred.borderRatio > kMaxBorderRatio)
        ok &= fail(name, "borderRatio=" + std::to_string(blurred.borderRatio) + " > " + std::to_string(kMaxBorderRatio));
    else
        ok &= pass(name);

    name = viewName + ": blurred AO does not repeat at the 4-texel tile period";
    if (blurred.lag4Corr > blurred.lag1Corr + kMaxLag4Excess)
        ok &= fail(name, "lag4Corr=" + std::to_string(blurred.lag4Corr) +
                         " lag1Corr=" + std::to_string(blurred.lag1Corr));
    else
        ok &= pass(name);

    return ok;
}

int main()
{
    std::cout.setf(std::ios::fixed);
    std::cout.precision(6);

    auto kernel = generateKernel();
    auto noise = generateNoise();
    glm::mat4 proj = makeProjection();
    glm::mat4 invProj = glm::inverse(proj);

    bool ok = true;
    {
        // FPS eye-level view: camera 1.7 m above the ground, pitched
        // 4 degrees down, so the ground fills the lower frame at grazing
        // angles.
        float pitch = glm::radians(4.0f);
        glm::vec3 eye(0.0f, 1.7f, 8.0f);
        glm::vec3 target(0.0f, 1.7f - 8.0f * std::tan(pitch), 0.0f);
        Scene scene = renderGround(eye, target, proj, invProj);
        ok &= checkView("grazing-fps", scene, kernel, noise, proj, invProj);
    }
    {
        // Editor-style orbit view: camera above the ground looking down
        // at roughly 51 degrees.
        glm::vec3 eye(0.0f, 16.0f, 13.0f);
        glm::vec3 target(0.0f, 0.0f, 0.0f);
        Scene scene = renderGround(eye, target, proj, invProj);
        ok &= checkView("orbit-editor", scene, kernel, noise, proj, invProj);
    }

    std::cout << (ok ? "all SSAO noise tests passed" : "SSAO noise tests FAILED") << std::endl;
    return ok ? 0 : 1;
}


