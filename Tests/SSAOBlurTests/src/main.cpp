// Regression tests for the SSAO bilateral blur depth step
// (assets/shaders/slang/ssao_blur.slang, threshold fed by the
// D3D12/Metal/Vulkan post-process builders). Run: SSAOBlurTests.exe
// Exit code 0 means all tests passed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kW = 960;   // half-res width  (1920 / 2)
constexpr int kH = 540;   // half-res height (1080 / 2)
constexpr float kNear = 0.1f;    // perspectiveRH_ZO near (Engine backends)
constexpr float kFar = 1000.0f;  // perspectiveRH_ZO far  (Engine backends)
constexpr float kFovYDeg = 70.0f;
constexpr float kDepthThresholdD3DMetal = 0.001f;  // D3D12 + Metal builders
constexpr float kDepthThresholdVulkan = 0.005f;    // Vulkan builder

// Gaussian weights copied verbatim from ssao_blur.slang.
constexpr float kGauss[5] = {0.227027f, 0.194596f, 0.121622f, 0.054054f, 0.016216f};

bool approx(float a, float b, float eps) { return std::abs(a - b) <= eps; }

bool fail(const std::string& name, const std::string& reason) {
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}
bool pass(const std::string& name) {
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

// Raw RH_ZO depth for a view-space forward distance d, matching
// glm::perspectiveRH_ZO(near=kNear, far=kFar) used by every backend:
// clip.z / clip.w with depth in [0,1], 0 at near and 1 at far.
float rawDepthFromViewDist(float d) {
    if (d <= kNear) return 0.0f;
    if (d >= kFar) return 1.0f;
    return kFar * (d - kNear) / ((kFar - kNear) * d);
}

// Inverse of the above, i.e. the view distance ReconstructViewPos in
// ssao.slang recovers through uInvProjection (up to the w divide).
float viewDistFromRawDepth(float depth) {
    if (depth <= 0.0f) return kNear;
    if (depth >= 1.0f) return kFar;
    return kNear * kFar / (kFar - depth * (kFar - kNear));
}

// HLSL step(edge, x): 1 when x >= edge, else 0. The shader calls
// step(abs(neighbourDepth - centerDepth), uDepthThreshold), so a tap is
// accepted exactly when abs(depth difference) <= uDepthThreshold.
float stepHlsl(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }

struct Image {
    std::vector<float> px;  // kW * kH, row 0 = top of image
    float& at(int x, int y) { return px[static_cast<size_t>(y) * kW + x]; }
    float at(int x, int y) const { return px[static_cast<size_t>(y) * kW + x]; }
};

float noise01(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// Floor plane y=0 seen from height camH with the camera pitched down by
// pitchDownDeg degrees. Returns the raw-depth image plus a floor mask.
struct Scene {
    Image depth;
    std::vector<char> floorMask;
    float camH;
    float pitchDownDeg;
};

Scene makeFloorScene(float camH, float pitchDownDeg) {
    Scene s;
    s.depth.px.assign(static_cast<size_t>(kW) * kH, 1.0f);
    s.floorMask.assign(static_cast<size_t>(kW) * kH, 0);
    s.camH = camH;
    s.pitchDownDeg = pitchDownDeg;
    const float pi = 3.14159265358979323846f;
    const float tanV = std::tan(kFovYDeg * 0.5f * pi / 180.0f);
    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    const float tanH = tanV * aspect;
    const float p = pitchDownDeg * pi / 180.0f;
    const float sinP = std::sin(p), cosP = std::cos(p);
    (void)tanH;
    for (int y = 0; y < kH; ++y) {
        // Row 0 is the top of the image: ndcY = +1 there.
        float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
        float dirY = ndcY * tanV;  // view-space ray y, ray z = -1
        // World ray y component after pitching down: dirY*cosP - sinP.
        float denom = sinP - dirY * cosP;  // >0 only below the horizon
        for (int x = 0; x < kW; ++x) {
            size_t i = static_cast<size_t>(y) * kW + x;
            if (denom <= 0.0f) continue;  // sky
            float viewDist = camH / denom;  // distance along camera forward
            if (viewDist >= kFar) continue;
            s.depth.px[i] = rawDepthFromViewDist(viewDist);
            s.floorMask[i] = 1;
        }
    }
    return s;
}

Image makeAOInput(const Scene& s) {
    Image ao;
    ao.px.assign(static_cast<size_t>(kW) * kH, 1.0f);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            size_t i = static_cast<size_t>(y) * kW + x;
            if (!s.floorMask[i]) continue;
            ao.px[i] = 0.80f + (noise01(x, y) - 0.5f) * 0.30f;
        }
    return ao;
}

struct BlurStats {
    double dropoutRate = 0.0;   // rejected taps / total taps (floor centres)
    double meanTotalWeight = 0.0;
    std::vector<double> dropoutPerRow;  // kH entries, -1 = no floor centre
};

// One separable pass of ssao_blur.slang along (dirX, dirY) in texels,
// with depth sampled from the (unchanged) depth image, clamped edges.
Image blurPass(const Image& in, const Image& depth, int dirX, int dirY,
               float threshold, BlurStats* stats) {
    Image out;
    out.px.assign(in.px.size(), 1.0f);
    if (stats) {
        stats->dropoutPerRow.assign(kH, -1.0);
        std::vector<long long> rowTaps(kH, 0), rowDrop(kH, 0);
        long long taps = 0, drop = 0;
        stats->dropoutPerRow = std::vector<double>(kH, -1.0);
        double weightSum = 0.0;
        long long centres = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                float centerDepth = depth.at(x, y);
                bool floorCentre = centerDepth < 0.9999f;
                float result = in.at(x, y) * kGauss[0];
                float totalWeight = kGauss[0];
                for (int i = 1; i < 5; ++i) {
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        int nx = std::clamp(x + sgn * i * dirX, 0, kW - 1);
                        int ny = std::clamp(y + sgn * i * dirY, 0, kH - 1);
                        float w = kGauss[i] * stepHlsl(std::abs(depth.at(nx, ny) - centerDepth), threshold);
                        result += in.at(nx, ny) * w;
                        totalWeight += w;
                        if (floorCentre) {
                            ++taps;
                            ++rowTaps[y];
                            if (w == 0.0f) { ++drop; ++rowDrop[y]; }
                        }
                    }
                }
                out.at(x, y) = result / std::max(totalWeight, 0.0001f);
                if (floorCentre) { weightSum += totalWeight; ++centres; }
            }
        }
        (void)rowTaps;
        if (taps > 0) stats->dropoutRate = static_cast<double>(drop) / static_cast<double>(taps);
        if (centres > 0) stats->meanTotalWeight = weightSum / static_cast<double>(centres);
        for (int y = 0; y < kH; ++y)
            if (rowTaps[y] > 0)
                stats->dropoutPerRow[y] = static_cast<double>(rowDrop[y]) / static_cast<double>(rowTaps[y]);
    } else {
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                float centerDepth = depth.at(x, y);
                float result = in.at(x, y) * kGauss[0];
                float totalWeight = kGauss[0];
                for (int i = 1; i < 5; ++i) {
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        int nx = std::clamp(x + sgn * i * dirX, 0, kW - 1);
                        int ny = std::clamp(y + sgn * i * dirY, 0, kH - 1);
                        float w = kGauss[i] * stepHlsl(std::abs(depth.at(nx, ny) - centerDepth), threshold);
                        result += in.at(nx, ny) * w;
                        totalWeight += w;
                    }
                }
                out.at(x, y) = result / std::max(totalWeight, 0.0001f);
            }
        }
    }
    return out;
}

// Full engine order: horizontal pass into an intermediate, then vertical.
Image blurFull(const Image& ao, const Image& depth, float threshold,
               BlurStats* statsH, BlurStats* statsV) {
    Image h = blurPass(ao, depth, 1, 0, threshold, statsH);
    return blurPass(h, depth, 0, 1, threshold, statsV);
}

// High-frequency row-to-row variation of the per-row mean AO: RMS of the
// row means minus their 9-row moving average, over rows mostly floor.
double stripeMetric(const Image& img, const Scene& s) {
    std::vector<double> means;
    for (int y = 0; y < kH; ++y) {
        double sum = 0.0;
        long long n = 0;
        for (int x = 0; x < kW; ++x)
            if (s.floorMask[static_cast<size_t>(y) * kW + x]) { sum += img.at(x, y); ++n; }
        if (n > kW / 2) means.push_back(sum / static_cast<double>(n));
    }
    if (means.size() < 16) return 0.0;
    double acc = 0.0;
    long long cnt = 0;
    for (size_t i = 4; i + 4 < means.size(); ++i) {
        double avg = 0.0;
        for (int k = -4; k <= 4; ++k) avg += means[i + k];
        avg /= 9.0;
        double d = means[i] - avg;
        acc += d * d;
        ++cnt;
    }
    return cnt ? std::sqrt(acc / static_cast<double>(cnt)) : 0.0;
}

struct ViewReport {
    std::string label;
    double maxStep1 = 0, maxStep4 = 0;
    double dropoutH = 0, dropoutV = 0, dropoutVmaxRow = 0;
    double weightH = 0, weightV = 0;
    double stripeBilateral = 0, stripeIdeal = 0, stripeRaw = 0;
};

ViewReport measureView(const std::string& label, float camH, float pitchDown, float threshold) {
    Scene scene = makeFloorScene(camH, pitchDown);
    Image ao = makeAOInput(scene);
    BlurStats sh, sv;
    Image blurred = blurFull(ao, scene.depth, threshold, &sh, &sv);
    Image ideal = blurFull(ao, scene.depth, 1e9f, nullptr, nullptr);
    ViewReport r;
    r.label = label;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            size_t i = static_cast<size_t>(y) * kW + x;
            if (!scene.floorMask[i]) continue;
            if (y + 1 < kH && scene.floorMask[i + kW])
                r.maxStep1 = std::max(r.maxStep1, static_cast<double>(std::abs(scene.depth.px[i + kW] - scene.depth.px[i])));
            if (y + 4 < kH && scene.floorMask[i + 4 * kW])
                r.maxStep4 = std::max(r.maxStep4, static_cast<double>(std::abs(scene.depth.px[i + 4 * kW] - scene.depth.px[i])));
        }
    std::printf("[METRIC] view=%s maxAbsDepthStep1=%.6f maxAbsDepthStep4=%.6f threshold=%.4f step4Margin=%.2fx\n",
                label.c_str(), r.maxStep1, r.maxStep4, threshold,
                r.maxStep4 > 0 ? threshold / r.maxStep4 : 0.0);
    r.dropoutH = sh.dropoutRate;
    r.dropoutV = sv.dropoutRate;
    r.weightH = sh.meanTotalWeight;
    r.weightV = sv.meanTotalWeight;
    for (double d : sv.dropoutPerRow) r.dropoutVmaxRow = std::max(r.dropoutVmaxRow, d);
    r.stripeBilateral = stripeMetric(blurred, scene);
    r.stripeIdeal = stripeMetric(ideal, scene);
    r.stripeRaw = stripeMetric(ao, scene);
    std::printf("[METRIC] view=%s camH=%.2f pitchDown=%.1fdeg threshold=%.4f "
                "dropoutH=%.4f dropoutV=%.4f dropoutV_worstRow=%.4f "
                "meanWeightH=%.4f meanWeightV=%.4f stripeRaw=%.6f "
                "stripeBilateral=%.6f stripeIdeal=%.6f stripeRatio=%.3f\n",
                label.c_str(), camH, pitchDown, threshold, r.dropoutH, r.dropoutV,
                r.dropoutVmaxRow, r.weightH, r.weightV, r.stripeRaw, r.stripeBilateral,
                r.stripeIdeal, r.stripeIdeal > 0 ? r.stripeBilateral / r.stripeIdeal : 0.0);
    std::printf("[METRIC] view=%s dropoutV_perRow:", label.c_str());
    for (int y = 30; y < kH; y += 60)
        if (sv.dropoutPerRow[y] >= 0) std::printf(" row%d=%.3f", y, sv.dropoutPerRow[y]);
    std::printf("\n");
    return r;
}

bool testDepthRoundTrip() {
    const std::string name = "RH_ZO depth reconstructs the view distance it was written from";
    for (float d : {0.5f, 2.0f, 10.0f, 50.0f, 200.0f, 800.0f}) {
        float back = viewDistFromRawDepth(rawDepthFromViewDist(d));
        if (!approx(back, d, 0.002f * d + 0.001f))
            return fail(name, "round-trip mismatch");
    }
    return pass(name);
}

bool testDepthEdgeIsRejected() {
    const std::string name = "a real depth discontinuity is rejected by the same step";
    float nearD = rawDepthFromViewDist(2.0f);
    float farD = rawDepthFromViewDist(20.0f);
    if (stepHlsl(std::abs(farD - nearD), kDepthThresholdD3DMetal) != 0.0f)
        return fail(name, "2 m vs 20 m step was accepted");
    if (stepHlsl(std::abs(rawDepthFromViewDist(2.02f) - nearD), kDepthThresholdD3DMetal) != 1.0f)
        return fail(name, "2 m vs 2.02 m step was rejected");
    return pass(name);
}

bool checkView(const ViewReport& r, const std::string& tag) {
    bool ok = true;
    {
        const std::string name = tag + ": same-surface vertical taps are mostly accepted";
        if (r.dropoutV > 0.50)
            ok &= fail(name, "vertical dropout " + std::to_string(r.dropoutV) + " exceeds 0.50");
        else
            ok &= pass(name);
    }
    {
        const std::string name = tag + ": blurred AO has no row striping beyond the fully blurred result";
        double limit = std::max(1.5 * r.stripeIdeal, r.stripeIdeal + 0.002);
        if (r.stripeBilateral > limit)
            ok &= fail(name, "stripe " + std::to_string(r.stripeBilateral) +
                                 " exceeds limit " + std::to_string(limit));
        else
            ok &= pass(name);
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= testDepthRoundTrip();
    ok &= testDepthEdgeIsRejected();

    // Editor-style orbit: elevated camera looking steeply down at the floor.
    ViewReport orbit = measureView("orbit", 12.0f, 50.0f, kDepthThresholdD3DMetal);
    // FPS eye level: 1.7 m eye height, near-horizontal view of the floor.
    ViewReport grazing = measureView("grazing", 1.7f, 2.0f, kDepthThresholdD3DMetal);
    // Same views under the larger Vulkan threshold, for comparison.
    measureView("orbit-vulkan-threshold", 12.0f, 50.0f, kDepthThresholdVulkan);
    measureView("grazing-vulkan-threshold", 1.7f, 2.0f, kDepthThresholdVulkan);

    ok &= checkView(orbit, "orbit view");
    ok &= checkView(grazing, "grazing view");

    std::cout << (ok ? "all SSAO blur tests passed" : "SSAO blur tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
