// SSAO host-input comparison and shader-math measurement tests.
// Part A feeds the shared post-process graph topology (used by both the
// standalone endFrame path and the editor endSceneRender path) through a
// counting fake backend and records the exact SSAO pass inputs each host
// configuration produces. Part B replicates the SSAO shader math from
// assets/shaders/slang/ssao.slang and ssao_blur.slang in C++ on synthetic
// depth and measures the resulting occlusion on ground that has no
// occluder above it. Run: SSAOInputTests.exe, exit 0 means all passed.

#include "Graphics/RenderGraph/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static bool fail(const std::string& name, const std::string& reason)
{
    std::cout << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Part A: render graph host-input comparison
// ---------------------------------------------------------------------------

struct FakeContext : RGContext {};

struct RecordedBarrier {
    RGResourceHandle handle;
    RGResourceUsage from;
    RGResourceUsage to;
};

struct FakeBackend : RGBackend {
    FakeContext ctx;
    int beginCount = 0;
    int endCount = 0;
    int flushCount = 0;
    std::vector<RGResourceHandle> created;
    std::vector<RGResourceHandle> destroyed;
    std::vector<RecordedBarrier> barriers;

    void createTransientTexture(RGResourceHandle handle, const RGTextureDesc&) override { created.push_back(handle); }
    void destroyTransientTexture(RGResourceHandle handle) override { destroyed.push_back(handle); }
    void insertBarrier(RGResourceHandle handle, RGResourceUsage fromUsage, RGResourceUsage toUsage) override
    {
        barriers.push_back({handle, fromUsage, toUsage});
    }
    void flushBarriers() override { ++flushCount; }
    RGContext& getContext() override { return ctx; }
    void beginFrame() override { ++beginCount; }
    void endFrame() override { ++endCount; }
};

struct HostConfig {
    const char* label;
    uint32_t width;
    uint32_t height;
    bool wantSSAO;
    bool renderImGui;
    bool renderRml;
};

struct RecordedFrame {
    std::vector<std::string> order;
    uint32_t ssaoWidth = 0;
    uint32_t ssaoHeight = 0;
    uint32_t depthIndex = UINT32_MAX;
    uint32_t ssaoRawIndex = UINT32_MAX;
    uint32_t tonemapSampledIndex = UINT32_MAX;
    uint32_t culled = 0;
    uint32_t passCount = 0;
};

// Mirrors PostProcessGraphBuilder::addPostProcessPasses pass declarations
// and the D3D12 builder import sizes (SSAO targets are the half-resolution
// pass outputs, floored by D3D12PostProcessPass::computeDimensions).
static RecordedFrame runHostGraph(const HostConfig& cfg)
{
    RenderGraph g;
    FakeBackend backend;
    RecordedFrame rec;
    g.setReferenceResolution(cfg.width, cfg.height);

    RGTextureDesc fullDesc;
    fullDesc.width = cfg.width;
    fullDesc.height = cfg.height;
    fullDesc.format = RGFormat::RGBA16_FLOAT;
    fullDesc.debugName = "OffscreenHDR";
    RGTextureHandle hdr = g.importTexture("OffscreenHDR", fullDesc, RGResourceUsage::RenderTarget);

    RGTextureDesc depthDesc;
    depthDesc.width = cfg.width;
    depthDesc.height = cfg.height;
    depthDesc.format = RGFormat::D24_UNORM_S8_UINT;
    depthDesc.debugName = "DepthBuffer";
    RGTextureHandle depth = g.importTexture("DepthBuffer", depthDesc, RGResourceUsage::DepthStencilWrite);

    RGTextureDesc outDesc = fullDesc;
    outDesc.format = RGFormat::RGBA8_UNORM;
    outDesc.debugName = "OutputTarget";
    RGTextureHandle output = g.importTexture("OutputTarget", outDesc, RGResourceUsage::Present);

    const uint32_t halfW = std::max(1u, static_cast<uint32_t>(cfg.width * 0.5f));
    const uint32_t halfH = std::max(1u, static_cast<uint32_t>(cfg.height * 0.5f));
    rec.ssaoWidth = halfW;
    rec.ssaoHeight = halfH;
    rec.depthIndex = depth.handle.index;

    RGTextureDesc ssaoDesc;
    ssaoDesc.width = halfW;
    ssaoDesc.height = halfH;
    ssaoDesc.format = RGFormat::R8_UNORM;
    ssaoDesc.debugName = "SSAORaw";
    RGTextureHandle ssaoRaw = g.importTexture("SSAORaw", ssaoDesc, RGResourceUsage::RenderTarget);
    ssaoDesc.debugName = "SSAOBlurH";
    RGTextureHandle ssaoBlurH = g.importTexture("SSAOBlurH", ssaoDesc, RGResourceUsage::RenderTarget);
    ssaoDesc.debugName = "SSAOBlurV";
    RGTextureHandle ssaoBlurV = g.importTexture("SSAOBlurV", ssaoDesc, RGResourceUsage::RenderTarget);
    rec.ssaoRawIndex = ssaoRaw.handle.index;

    if (cfg.wantSSAO) {
        g.addPass("SSAO",
                  [&](RGBuilder& b) { b.read(depth, RGResourceUsage::ShaderResource); b.write(ssaoRaw, RGResourceUsage::RenderTarget); },
                  [&, ssaoRaw](RGContext&) { rec.order.push_back("SSAO"); });
        g.addPass("SSAO Blur H",
                  [&](RGBuilder& b) { b.read(ssaoRaw, RGResourceUsage::ShaderResource); b.read(depth, RGResourceUsage::ShaderResource); b.write(ssaoBlurH, RGResourceUsage::RenderTarget); },
                  [&, ssaoBlurH](RGContext&) { rec.order.push_back("SSAO Blur H"); });
        g.addPass("SSAO Blur V",
                  [&](RGBuilder& b) { b.read(ssaoBlurH, RGResourceUsage::ShaderResource); b.read(depth, RGResourceUsage::ShaderResource); b.write(ssaoBlurV, RGResourceUsage::RenderTarget); },
                  [&, ssaoBlurV](RGContext&) { rec.order.push_back("SSAO Blur V"); });
    }

    g.addPass("Tonemapping",
              [&](RGBuilder& b) {
                  b.read(hdr, RGResourceUsage::ShaderResource);
                  if (cfg.wantSSAO) b.read(ssaoBlurV, RGResourceUsage::ShaderResource);
                  b.write(output, RGResourceUsage::RenderTarget);
                  b.setSideEffect();
              },
              [&, ssaoBlurV](RGContext&) {
                  rec.order.push_back("Tonemapping");
                  if (cfg.wantSSAO) rec.tonemapSampledIndex = ssaoBlurV.handle.index;
              });

    if (cfg.renderRml) {
        g.addPass("RmlUi",
                  [&](RGBuilder& b) { b.write(output, RGResourceUsage::RenderTarget); b.setSideEffect(); },
                  [&](RGContext&) { rec.order.push_back("RmlUi"); });
    }
    if (cfg.renderImGui) {
        g.addPass("ImGui",
                  [&](RGBuilder& b) { b.write(output, RGResourceUsage::RenderTarget); b.setSideEffect(); },
                  [&](RGContext&) { rec.order.push_back("ImGui"); });
    }
    g.addPass("Present",
              [&](RGBuilder& b) { b.read(output, RGResourceUsage::Present); b.setSideEffect(); },
              [&](RGContext&) { rec.order.push_back("Present"); });
    g.addPass("DepthRestore",
              [&](RGBuilder& b) { b.write(depth, RGResourceUsage::DepthStencilWrite); b.setSideEffect(); },
              [&](RGContext&) { rec.order.push_back("DepthRestore"); });

    if (!g.compile()) return rec;
    rec.culled = g.getStats().culled_count;
    rec.passCount = g.getStats().pass_count;
    g.execute(backend);
    return rec;
}

static std::vector<std::string> ssaoSubsequence(const std::vector<std::string>& order)
{
    std::vector<std::string> out;
    for (const auto& s : order)
        if (s.rfind("SSAO", 0) == 0 || s == "Tonemapping") out.push_back(s);
    return out;
}

// Enable formulas transcribed from the two host code paths.
// Standalone (D3D12RenderAPI_Frame.cpp endFrame):
//   wantSSAO = sizeMatch && ssaoEnabled && blurV.isInitialized()
// Editor/PIE (D3D12RenderAPI_Viewport.cpp endSceneRender):
//   wantSSAO = sizeMatch && ssaoEnabled && blurV.isInitialized()
//              && depthSRV != UINT(-1)
static bool wantSSAOStandalone(bool sizeMatch, bool enabled, bool blurInit, bool /*depthValid*/)
{
    return sizeMatch && enabled && blurInit;
}
static bool wantSSAOEditor(bool sizeMatch, bool enabled, bool blurInit, bool depthValid)
{
    return sizeMatch && enabled && blurInit && depthValid;
}

static bool testHostSSAOGraphInputs()
{
    const std::string name = "standalone and editor graphs feed SSAO identical inputs at equal size";
    HostConfig standalone{"standalone", 1920, 1080, true, true, true};
    HostConfig editor{"editor", 1920, 1080, true, false, false};
    RecordedFrame a = runHostGraph(standalone);
    RecordedFrame b = runHostGraph(editor);

    std::cout << "  standalone order:";
    for (auto& s : a.order) std::cout << " " << s;
    std::cout << "\n  editor order:    ";
    for (auto& s : b.order) std::cout << " " << s;
    std::cout << "\n  standalone ssao=" << a.ssaoWidth << "x" << a.ssaoHeight
              << " depthIdx=" << a.depthIndex << " tonemapSampleIdx=" << a.tonemapSampledIndex
              << " culled=" << a.culled
              << "\n  editor     ssao=" << b.ssaoWidth << "x" << b.ssaoHeight
              << " depthIdx=" << b.depthIndex << " tonemapSampleIdx=" << b.tonemapSampledIndex
              << " culled=" << b.culled << std::endl;

    if (ssaoSubsequence(a.order) != ssaoSubsequence(b.order))
        return fail(name, "SSAO pass order differs between hosts");
    if (a.ssaoWidth != b.ssaoWidth || a.ssaoHeight != b.ssaoHeight)
        return fail(name, "SSAO target size differs at equal cfg size");
    if (a.tonemapSampledIndex == UINT32_MAX || b.tonemapSampledIndex == UINT32_MAX)
        return fail(name, "tonemapping did not sample SSAO in one host");
    if (a.tonemapSampledIndex != b.tonemapSampledIndex)
        return fail(name, "tonemapping samples a different texture handle index");
    if (a.culled != 0 || b.culled != 0)
        return fail(name, "SSAO chain culled in one host");
    return pass(name);
}

static bool testHostSizeSource()
{
    const std::string name = "SSAO size follows each host cfg size source";
    HostConfig standalone{"standalone", 1920, 1080, true, true, true};
    HostConfig editor{"editor", 1280, 720, true, false, false};
    RecordedFrame a = runHostGraph(standalone);
    RecordedFrame b = runHostGraph(editor);
    std::cout << "  standalone cfg 1920x1080 -> ssao " << a.ssaoWidth << "x" << a.ssaoHeight
              << "; editor cfg 1280x720 -> ssao " << b.ssaoWidth << "x" << b.ssaoHeight << std::endl;
    if (a.ssaoWidth != 960 || a.ssaoHeight != 540) return fail(name, "standalone SSAO size is not floor(cfg/2)");
    if (b.ssaoWidth != 640 || b.ssaoHeight != 360) return fail(name, "editor SSAO size is not floor(cfg/2)");
    return pass(name);
}

static bool testEnableGuardDifference()
{
    const std::string name = "SSAO enable is identical when depth SRV is valid and diverges when invalid";
    bool sameValid = wantSSAOStandalone(true, true, true, true) == wantSSAOEditor(true, true, true, true);
    bool diffInvalid = wantSSAOStandalone(true, true, true, false) != wantSSAOEditor(true, true, true, false);
    std::cout << "  depth valid: standalone=" << wantSSAOStandalone(true, true, true, true)
              << " editor=" << wantSSAOEditor(true, true, true, true)
              << "; depth invalid: standalone=" << wantSSAOStandalone(true, true, true, false)
              << " editor=" << wantSSAOEditor(true, true, true, false) << std::endl;
    if (!sameValid) return fail(name, "enable differs with valid depth");
    if (!diffInvalid) return fail(name, "expected the editor-only depth guard to diverge");
    return pass(name);
}

static bool testSSAODisabledCullsChain()
{
    const std::string name = "wantSSAO false removes the SSAO chain and its tonemap sample";
    HostConfig cfg{"editor-nossao", 1280, 720, false, false, false};
    RecordedFrame r = runHostGraph(cfg);
    if (!ssaoSubsequence(r.order).empty() && ssaoSubsequence(r.order).size() != 1)
        return fail(name, "SSAO passes executed while disabled");
    if (r.tonemapSampledIndex != UINT32_MAX)
        return fail(name, "tonemapping sampled SSAO while disabled");
    return pass(name);
}

// ---------------------------------------------------------------------------
// Part B: SSAO shader math replicated from ssao.slang / ssao_blur.slang
// ---------------------------------------------------------------------------

struct Vec3 { float x = 0, y = 0, z = 0; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 normalize(Vec3 a) { float l = std::sqrt(dot(a, a)); return l > 1e-9f ? a * (1.0f / l) : a; }

struct Mat4 { float m[4][4] = {}; }; // column-major: m[col][row]

static Mat4 perspectiveRH_ZO(float fovRad, float aspect, float zn, float zf)
{
    Mat4 r;
    float f = 1.0f / std::tan(fovRad * 0.5f);
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = zf / (zn - zf);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (zf * zn) / (zn - zf);
    return r;
}

static Mat4 inverse(const Mat4& a)
{
    // Gauss-Jordan on row-major view of the column-major storage.
    double aug[4][8] = {};
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) aug[row][col] = a.m[col][row];
        aug[row][4 + row] = 1.0;
    }
    for (int col = 0, pivRow = 0; col < 4 && pivRow < 4; col++, pivRow++) {
        int sel = pivRow;
        for (int r = pivRow + 1; r < 4; r++)
            if (std::abs(aug[r][col]) > std::abs(aug[sel][col])) sel = r;
        std::swap(aug[sel], aug[pivRow]);
        double d = aug[pivRow][col];
        for (int j = 0; j < 8; j++) aug[pivRow][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == pivRow) continue;
            double f = aug[r][col];
            for (int j = 0; j < 8; j++) aug[r][j] -= f * aug[pivRow][j];
        }
    }
    Mat4 out;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) out.m[col][row] = static_cast<float>(aug[row][4 + col]);
    return out;
}

static Vec3 mulPoint(const Mat4& m, Vec3 v, float& wOut)
{
    float x = m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z + m.m[3][0];
    float y = m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z + m.m[3][1];
    float z = m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z + m.m[3][2];
    wOut = m.m[0][3] * v.x + m.m[1][3] * v.y + m.m[2][3] * v.z + m.m[3][3];
    return {x, y, z};
}

static constexpr int FULL_W = 640;
static constexpr int FULL_H = 360;
static constexpr int HALF_W = FULL_W / 2;
static constexpr int HALF_H = FULL_H / 2;

struct Camera {
    Vec3 eye, right, up, fwd; // fwd is world look direction
    Mat4 proj, invProj;
};

static Camera makeCamera(Vec3 eye, Vec3 target, float fovDeg)
{
    Camera c;
    c.eye = eye;
    c.fwd = normalize(target - eye);
    c.right = normalize(cross(c.fwd, {0, 1, 0}));
    c.up = cross(c.right, c.fwd);
    c.proj = perspectiveRH_ZO(fovDeg * 3.14159265f / 180.0f,
                               static_cast<float>(FULL_W) / FULL_H, 0.1f, 1000.0f);
    c.invProj = inverse(c.proj);
    return c;
}

// View-space position of a world point (view forward is -Z).
static Vec3 worldToView(const Camera& c, Vec3 p)
{
    Vec3 d = p - c.eye;
    return {dot(c.right, d), dot(c.up, d), -dot(c.fwd, d)};
}

// Renders depth (D24-quantized) and a kind mask: 0 sky, 1 ground, 2 box.
static void renderDepth(const Camera& cam, bool withBox,
                        std::vector<float>& depth, std::vector<uint8_t>& kind)
{
    depth.assign(FULL_W * FULL_H, 1.0f);
    kind.assign(FULL_W * FULL_H, 0);
    for (int y = 0; y < FULL_H; y++) {
        for (int x = 0; x < FULL_W; x++) {
            float ndcX = ((x + 0.5f) / FULL_W) * 2.0f - 1.0f;
            float ndcY = 1.0f - ((y + 0.5f) / FULL_H) * 2.0f;
            Vec3 dv{ndcX / cam.proj.m[0][0], ndcY / cam.proj.m[1][1], -1.0f};
            Vec3 dir = cam.right * dv.x + cam.up * dv.y + cam.fwd * 1.0f;
            float bestT = 1e30f;
            uint8_t bestKind = 0;
            if (dir.y < -1e-6f) {
                float t = -cam.eye.y / dir.y;
                if (t > 0) { bestT = t; bestKind = 1; }
            }
            if (withBox) {
                const Vec3 bmin{-0.6f, 0.0f, -4.2f}, bmax{0.6f, 2.2f, -3.0f};
                float tmin = -1e30f, tmax = 1e30f;
                bool ok = true;
                const float o[3] = {cam.eye.x, cam.eye.y, cam.eye.z};
                const float d[3] = {dir.x, dir.y, dir.z};
                const float lo[3] = {bmin.x, bmin.y, bmin.z};
                const float hi[3] = {bmax.x, bmax.y, bmax.z};
                for (int a = 0; a < 3; a++) {
                    if (std::abs(d[a]) < 1e-9f) {
                        if (o[a] < lo[a] || o[a] > hi[a]) ok = false;
                        continue;
                    }
                    float t0 = (lo[a] - o[a]) / d[a], t1 = (hi[a] - o[a]) / d[a];
                    if (t0 > t1) std::swap(t0, t1);
                    tmin = std::max(tmin, t0);
                    tmax = std::min(tmax, t1);
                }
                if (ok && tmin <= tmax && tmin > 0 && tmin < bestT) { bestT = tmin; bestKind = 2; }
            }
            if (bestKind != 0) {
                Vec3 view = worldToView(cam, cam.eye + dir * bestT);
                float w = 0;
                Vec3 clip = mulPoint(cam.proj, view, w);
                float d = clip.z / w;
                d = std::floor(d * 16777215.0f + 0.5f) / 16777215.0f;
                depth[y * FULL_W + x] = d;
                kind[y * FULL_W + x] = bestKind;
            }
        }
    }
}

static float sampleDepth(const std::vector<float>& depth, float u, float v)
{
    int ix = std::clamp(static_cast<int>(u * FULL_W), 0, FULL_W - 1);
    int iy = std::clamp(static_cast<int>(v * FULL_H), 0, FULL_H - 1);
    return depth[iy * FULL_W + ix];
}

static Vec3 reconstructViewPos(const Camera& cam, float u, float v, float d)
{
    float ndcX = u * 2.0f - 1.0f;
    float ndcY = v * 2.0f - 1.0f;
    float w = 0;
    Vec3 vpos = mulPoint(cam.invProj, {ndcX, -ndcY, d}, w);
    (void)w;
    float wp = cam.invProj.m[0][3] * ndcX + cam.invProj.m[1][3] * (-ndcY) +
               cam.invProj.m[2][3] * d + cam.invProj.m[3][3];
    return vpos * (1.0f / wp);
}

static float smoothstepf(float x)
{
    float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct SSAOResult {
    std::vector<float> ao;      // half resolution, after blur
    std::vector<float> aoRaw;   // half resolution, before blur
};

static SSAOResult runSSAO(const Camera& cam, const std::vector<float>& depth)
{
    // Kernel and noise use the engine generators and seeds.
    std::array<Vec3, 16> kernel{};
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> d01(0.0f, 1.0f);
        std::uniform_real_distribution<float> dn(-1.0f, 1.0f);
        for (int i = 0; i < 16; i++) {
            Vec3 s = normalize({dn(rng), dn(rng), d01(rng)});
            s = s * d01(rng);
            float scale = static_cast<float>(i) / 16.0f;
            scale = 0.1f + scale * scale * 0.9f;
            kernel[i] = s * scale;
        }
    }
    std::array<Vec3, 16> noise{};
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);
        for (int i = 0; i < 16; i++) {
            float x = d(rng), y = d(rng);
            float l = std::sqrt(x * x + y * y);
            if (l > 0.0001f) { x /= l; y /= l; }
            noise[i] = {x, y, 0.0f};
        }
    }

    const float radius = 0.5f, bias = 0.025f, power = 1.5f;
    const float depthTexelX = 1.0f / (HALF_W * 2.0f);
    const float depthTexelY = 1.0f / (HALF_H * 2.0f);

    SSAOResult res;
    res.aoRaw.assign(HALF_W * HALF_H, 1.0f);
    for (int y = 0; y < HALF_H; y++) {
        for (int x = 0; x < HALF_W; x++) {
            float u = (x + 0.5f) / HALF_W;
            float v = (y + 0.5f) / HALF_H;
            // SnapDepthUV in ssao.slang: centre of the full-res depth texel.
            auto snapU = [](float uu) { return (std::floor(uu * FULL_W) + 0.5f) / FULL_W; };
            auto snapV = [](float vv) { return (std::floor(vv * FULL_H) + 0.5f) / FULL_H; };
            float du = snapU(u), dv = snapV(v);
            float d0 = sampleDepth(depth, du, dv);
            if (d0 >= 0.9999f) continue;
            Vec3 frag = reconstructViewPos(cam, du, dv, d0);
            auto reconAt = [&](float uu, float vv) {
                float su = snapU(uu), sv = snapV(vv);
                return reconstructViewPos(cam, su, sv, sampleDepth(depth, su, sv));
            };
            Vec3 pR = reconAt(du + depthTexelX, dv), pL = reconAt(du - depthTexelX, dv);
            Vec3 pU = reconAt(du, dv + depthTexelY), pD = reconAt(du, dv - depthTexelY);
            Vec3 dx = (std::abs(pR.z - frag.z) < std::abs(frag.z - pL.z)) ? (pR - frag) : (frag - pL);
            Vec3 dy = (std::abs(pU.z - frag.z) < std::abs(frag.z - pD.z)) ? (pU - frag) : (frag - pD);
            Vec3 normal = normalize(cross(dy, dx));

            float nsX = HALF_W / 4.0f, nsY = HALF_H / 4.0f;
            int nix = std::clamp(static_cast<int>(std::fmod(u * nsX, 1.0f) * 4), 0, 3);
            int niy = std::clamp(static_cast<int>(std::fmod(v * nsY, 1.0f) * 4), 0, 3);
            Vec3 randomVec = noise[niy * 4 + nix];
            Vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
            Vec3 bitangent = cross(normal, tangent);

            float occlusion = 0.0f;
            for (int i = 0; i < 16; i++) {
                Vec3 sdir = tangent * kernel[i].x + bitangent * kernel[i].y + normal * kernel[i].z;
                Vec3 spos = frag + sdir * radius;
                float w = 0;
                Vec3 off = mulPoint(cam.proj, spos, w);
                off = off * (1.0f / w);
                float su = off.x * 0.5f + 0.5f;
                float sv = 1.0f - (off.y * 0.5f + 0.5f);
                Vec3 svp = reconstructViewPos(cam, su, sv, sampleDepth(depth, su, sv));
                float rangeCheck = smoothstepf(radius / std::max(std::abs(frag.z - svp.z), 0.0001f));
                occlusion += (svp.z >= spos.z + bias ? 1.0f : 0.0f) * rangeCheck;
            }
            float ao = 1.0f - occlusion / 16.0f;
            res.aoRaw[y * HALF_W + x] = std::pow(std::clamp(ao, 0.0f, 1.0f), power);
        }
    }

    // Two-pass bilateral blur from ssao_blur.slang (depth threshold 0.001).
    const float gw[5] = {0.227027f, 0.194596f, 0.121622f, 0.054054f, 0.016216f};
    std::vector<float> cur = res.aoRaw, next(HALF_W * HALF_H, 1.0f);
    for (int passIdx = 0; passIdx < 2; passIdx++) {
        float bx = passIdx == 0 ? 1.0f / HALF_W : 0.0f;
        float by = passIdx == 0 ? 0.0f : 1.0f / HALF_H;
        for (int y = 0; y < HALF_H; y++) {
            for (int x = 0; x < HALF_W; x++) {
                float u = (x + 0.5f) / HALF_W;
                float v = (y + 0.5f) / HALF_H;
                float centerAO = cur[y * HALF_W + x];
                float centerDepth = sampleDepth(depth, u, v);
                float result = centerAO * gw[0];
                float total = gw[0];
                for (int i = 1; i < 5; i++) {
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        float uu = u + sgn * bx * i;
                        float vv = v + sgn * by * i;
                        int sx = std::clamp(static_cast<int>(uu * HALF_W), 0, HALF_W - 1);
                        int sy = std::clamp(static_cast<int>(vv * HALF_H), 0, HALF_H - 1);
                        float wgt = gw[i] * (std::abs(sampleDepth(depth, uu, vv) - centerDepth) <= 0.001f ? 1.0f : 0.0f);
                        result += cur[sy * HALF_W + sx] * wgt;
                        total += wgt;
                    }
                }
                next[y * HALF_W + x] = result / std::max(total, 0.0001f);
            }
        }
        cur.swap(next);
    }
    res.ao = cur;
    return res;
}

struct ViewMetrics {
    double mean = 1, minAO = 1, falseOccPct = 0, dropoutPct = 0, stripe = 0;
    int groundPixels = 0;
};

static ViewMetrics groundMetrics(const SSAOResult& r, const std::vector<uint8_t>& kind, bool blurred)
{
    const std::vector<float>& ao = blurred ? r.ao : r.aoRaw;
    ViewMetrics m;
    double sum = 0, stripeSum = 0;
    int stripeCount = 0;
    for (int y = 0; y < HALF_H; y++) {
        for (int x = 0; x < HALF_W; x++) {
            int fx = std::min(FULL_W - 1, x * 2);
            int fy = std::min(FULL_H - 1, y * 2);
            if (kind[fy * FULL_W + fx] != 1) continue;
            float v = ao[y * HALF_W + x];
            sum += v;
            m.minAO = std::min(m.minAO, static_cast<double>(v));
            if (v < 0.95f) m.falseOccPct += 1;
            if (v < 0.5f) m.dropoutPct += 1;
            m.groundPixels++;
            if (y + 1 < HALF_H && kind[(fy + 2) * FULL_W + fx] == 1) {
                stripeSum += std::abs(v - ao[(y + 1) * HALF_W + x]);
                stripeCount++;
            }
        }
    }
    if (m.groundPixels > 0) {
        m.mean = sum / m.groundPixels;
        m.falseOccPct = 100.0 * m.falseOccPct / m.groundPixels;
        m.dropoutPct = 100.0 * m.dropoutPct / m.groundPixels;
    }
    m.stripe = stripeCount ? stripeSum / stripeCount : 0;
    return m;
}

static bool testView(const char* label, const Camera& cam)
{
    std::string name = std::string(label) + ": unoccluded ground keeps AO near 1";
    std::vector<float> depth;
    std::vector<uint8_t> kind;
    renderDepth(cam, false, depth, kind);
    SSAOResult r = runSSAO(cam, depth);
    ViewMetrics raw = groundMetrics(r, kind, false);
    ViewMetrics blurred = groundMetrics(r, kind, true);
    std::cout << "  " << label << " raw:     meanAO=" << raw.mean << " minAO=" << raw.minAO
              << " falseOcc=" << raw.falseOccPct << "% dropout=" << raw.dropoutPct
              << "% stripe=" << raw.stripe << " groundPx=" << raw.groundPixels << "\n"
              << "  " << label << " blurred: meanAO=" << blurred.mean << " minAO=" << blurred.minAO
              << " falseOcc=" << blurred.falseOccPct << "% dropout=" << blurred.dropoutPct
              << "% stripe=" << blurred.stripe << std::endl;
    const ViewMetrics& m = blurred;
    if (m.groundPixels < 1000) return fail(name, "too few ground pixels measured");
    if (m.mean < 0.98) return fail(name, "mean AO on unoccluded ground is below 0.98");
    if (m.falseOccPct > 2.0) return fail(name, "false occlusion above 2% on unoccluded ground");
    if (m.dropoutPct > 0.1) return fail(name, "dropout above 0.1% on unoccluded ground");
    if (m.stripe > 0.01) return fail(name, "row-to-row stripe metric above 0.01");
    return pass(name);
}

static bool testBoxContact(const Camera& cam)
{
    const std::string name = "box contact darkens nearby ground (true occlusion is detected)";
    std::vector<float> depth;
    std::vector<uint8_t> kind;
    renderDepth(cam, true, depth, kind);
    SSAOResult r = runSSAO(cam, depth);
    // Ground half-res pixels whose full-res source is ground: split by
    // image-space distance to a box pixel as a contact proxy.
    std::vector<uint8_t> boxHalf(HALF_W * HALF_H, 0);
    for (int y = 0; y < HALF_H; y++)
        for (int x = 0; x < HALF_W; x++)
            boxHalf[y * HALF_W + x] = kind[(y * 2) * FULL_W + x * 2] == 2 ? 1 : 0;
    double nearSum = 0, farSum = 0;
    int nearN = 0, farN = 0;
    for (int y = 0; y < HALF_H; y++) {
        for (int x = 0; x < HALF_W; x++) {
            if (kind[(y * 2) * FULL_W + x * 2] != 1) continue;
            int best = 1 << 28;
            for (int by = std::max(0, y - 24); by < std::min(HALF_H, y + 25); by++)
                for (int bx = std::max(0, x - 24); bx < std::min(HALF_W, x + 25); bx++)
                    if (boxHalf[by * HALF_W + bx]) best = std::min(best, std::abs(by - y) + std::abs(bx - x));
            if (best <= 3) { nearSum += r.ao[y * HALF_W + x]; nearN++; }
            else if (best > 30) { farSum += r.ao[y * HALF_W + x]; farN++; }
        }
    }
    double nearAO = nearN ? nearSum / nearN : 1.0;
    double farAO = farN ? farSum / farN : 1.0;
    std::cout << "  box contact: nearAO=" << nearAO << " (n=" << nearN << ") farAO=" << farAO
              << " (n=" << farN << ") delta=" << (farAO - nearAO) << std::endl;
    if (nearN < 20 || farN < 20) return fail(name, "too few contact/far ground pixels");
    if (farAO - nearAO < 0.03) return fail(name, "contact AO is not measurably darker than far ground");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testHostSSAOGraphInputs();
    ok &= testHostSizeSource();
    ok &= testEnableGuardDifference();
    ok &= testSSAODisabledCullsChain();

    Camera fps = makeCamera({0.0f, 1.7f, 0.0f}, {0.0f, 1.7f, -10.0f}, 75.0f);
    Camera orbit = makeCamera({0.0f, 7.0f, 7.0f}, {0.0f, 0.0f, 0.0f}, 75.0f);
    ok &= testView("FPS eye-level grazing", fps);
    ok &= testView("editor-style orbit", orbit);
    ok &= testBoxContact(fps);

    std::cout << (ok ? "all SSAO input tests passed" : "SSAO input tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
