// Tests for view-space normal reconstruction in assets/shaders/slang/ssao.slang
// (ReconstructViewPos / ReconstructNormal), as executed by the D3D12 and
// Vulkan backends at half resolution with a point-sampled full-res depth
// buffer. Run: SSAONormalTests.exe - exit code 0 means all tests passed.
//
// The shader functions are replicated line by line below and run over an
// analytic depth buffer rendered from a single known plane (the ground
// plane y = 0), so the true view-space normal is known at every texel.
// A correct reconstruction of a smooth plane gives a constant normal:
// near-zero angle error everywhere and near-zero texel-to-texel normal
// change. The checks below require that; any banding in the reconstructed
// normals shows up as angle error and as a period-2 (even/odd texel)
// stripe component and fails the checks. Error statistics cover texels
// whose whole reconstruction stencil lies on the plane; texels touching
// the sky or the screen border are counted separately as edge texels.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/vector_double3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kFullW = 1920;
constexpr int kFullH = 1080;
constexpr float kFovDeg = 75.0f;   // RenderAPI field_of_view default
constexpr float kNear = 0.1f;      // perspectiveRH_ZO near in backends
constexpr float kFar = 1000.0f;    // perspectiveRH_ZO far in backends

using Vec2f = glm::vec2;
using Vec3f = glm::vec3;
using Mat4f = glm::mat4;
using Vec3d = glm::dvec3;
using Mat4d = glm::dmat4;

// ---------------------------------------------------------------------------
// Shader replica: assets/shaders/slang/ssao.slang
// ReconstructViewPos (lines 50-64; the HLSL and Vulkan branches of the
// current source build the same clip position) and ReconstructNormal
// (lines 68-94, Vulkan branch cross(dx, dy); the HLSL branch swaps the
// cross operands, which flips the sign only and does not change the
// unsigned angle measured here). Depth sampling follows the backends:
// D3D12 uses MIN_MAG_MIP_POINT clamp samplers (D3D12RenderAPI_SSAO.cpp)
// and Vulkan uses VK_FILTER_NEAREST clamp-to-edge samplers
// (VulkanRenderAPI_SSAO.cpp), so Sample() is a clamped nearest texel.
// ---------------------------------------------------------------------------

template <typename T>
using Vec3 = glm::vec<3, T, glm::defaultp>;
template <typename T>
using Mat4 = glm::mat<4, 4, T, glm::defaultp>;

template <typename T>
Vec3<T> ReconstructViewPos(glm::vec<2, T, glm::defaultp> uv, T depth, const Mat4<T>& invProj)
{
    glm::vec<2, T, glm::defaultp> ndc = uv * T(2) - T(1);
    glm::vec<4, T, glm::defaultp> clipPos(ndc.x, -ndc.y, depth, T(1));
    glm::vec<4, T, glm::defaultp> viewPos = invProj * clipPos;
    return Vec3<T>(viewPos.x, viewPos.y, viewPos.z) / viewPos.w;
}

template <typename T>
T SampleDepthPoint(const std::vector<T>& depth, int w, int h, glm::vec<2, T, glm::defaultp> uv)
{
    int i = static_cast<int>(std::floor(static_cast<double>(uv.x) * w));
    int j = static_cast<int>(std::floor(static_cast<double>(uv.y) * h));
    i = std::clamp(i, 0, w - 1);
    j = std::clamp(j, 0, h - 1);
    return depth[static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)];
}

// SnapDepthUV in ssao.slang: moves a coordinate to the centre of the
// full-resolution depth texel it falls in, so the point-sampled texel and
// the position reconstructed from it agree. In the shipping configuration
// the depth size is twice the half-resolution screen size held in the
// shader constants; the replica keys the snap to the depth size directly.
template <typename T>
glm::vec<2, T, glm::defaultp> SnapDepthUV(glm::vec<2, T, glm::defaultp> uv)
{
    using Vec2 = glm::vec<2, T, glm::defaultp>;
    Vec2 depthSize{T(kFullW), T(kFullH)};
    return (glm::floor(uv * depthSize) + T(0.5)) / depthSize;
}

// Returns false when the cross product degenerates (zero-area stencil).
// sampleDepth(uv) supplies depthTexture.Sample(depthSampler, uv).
template <typename T, typename Sampler>
bool ReconstructNormal(glm::vec<2, T, glm::defaultp> uv, glm::vec<2, T, glm::defaultp> texelSize,
                       Vec3<T> viewPos, const Sampler& sampleDepth,
                       const Mat4<T>& invProj, Vec3<T>& outNormal)
{
    using Vec2 = glm::vec<2, T, glm::defaultp>;
    Vec2 uvRight = SnapDepthUV(uv + Vec2(texelSize.x, T(0)));
    Vec2 uvLeft  = SnapDepthUV(uv - Vec2(texelSize.x, T(0)));
    Vec2 uvUp    = SnapDepthUV(uv + Vec2(T(0), texelSize.y));
    Vec2 uvDown  = SnapDepthUV(uv - Vec2(T(0), texelSize.y));

    T depthRight = sampleDepth(uvRight);
    T depthLeft  = sampleDepth(uvLeft);
    T depthUp    = sampleDepth(uvUp);
    T depthDown  = sampleDepth(uvDown);

    Vec3<T> posRight = ReconstructViewPos(uvRight, depthRight, invProj);
    Vec3<T> posLeft  = ReconstructViewPos(uvLeft, depthLeft, invProj);
    Vec3<T> posUp    = ReconstructViewPos(uvUp, depthUp, invProj);
    Vec3<T> posDown  = ReconstructViewPos(uvDown, depthDown, invProj);

    Vec3<T> dx = (std::abs(posRight.z - viewPos.z) < std::abs(viewPos.z - posLeft.z))
                     ? (posRight - viewPos) : (viewPos - posLeft);
    Vec3<T> dy = (std::abs(posUp.z - viewPos.z) < std::abs(viewPos.z - posDown.z))
                     ? (posUp - viewPos) : (viewPos - posDown);

    Vec3<T> c = glm::cross(dx, dy);
    T len = glm::length(c);
    if (!(len > T(1e-12)) || !std::isfinite(static_cast<double>(len)))
        return false;
    outNormal = c / len;
    return true;
}

// ---------------------------------------------------------------------------
// Analytic depth for the ground plane y = 0 seen by a camera. Depth at a
// texel centre is the exact perspective depth of the plane/ray hit, the
// same value a rasterizer writes before storage quantization.
// ---------------------------------------------------------------------------

struct ViewSetup {
    const char* name;
    Mat4f view;
    Vec3f trueNormalView; // unit view-space plane normal, facing the camera
    double planeH;        // dot(trueNormalView, any view-space plane point)
};

ViewSetup MakeView(const char* name, Vec3f eye, Vec3f target)
{
    ViewSetup v;
    v.name = name;
    v.view = glm::lookAt(eye, target, Vec3f(0.0f, 1.0f, 0.0f));
    Vec3f n = glm::normalize(glm::mat3(v.view) * Vec3f(0.0f, 1.0f, 0.0f));
    // Camera origin in view space is (0,0,0); keep the normal facing it.
    glm::vec4 planePoint4 = v.view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    Vec3f pView(planePoint4.x, planePoint4.y, planePoint4.z);
    if (glm::dot(n, pView) > 0.0f) n = -n;
    v.trueNormalView = n;
    v.planeH = static_cast<double>(glm::dot(n, pView));
    return v;
}

// Fills an analytic depth buffer at full resolution in double precision.
// Texels that miss the plane or fall outside [near, far] get depth 1.
std::vector<double> RenderAnalyticDepth(const ViewSetup& view, const Mat4d& proj, const Mat4d& invProj)
{
    std::vector<double> depth(static_cast<size_t>(kFullW) * kFullH, 1.0);
    Vec3d n(view.trueNormalView);
    for (int j = 0; j < kFullH; ++j) {
        for (int i = 0; i < kFullW; ++i) {
            glm::dvec2 uv((i + 0.5) / kFullW, (j + 0.5) / kFullH);
            glm::dvec2 ndc = uv * 2.0 - 1.0;
            glm::dvec4 v4 = invProj * glm::dvec4(ndc.x, -ndc.y, 1.0, 1.0);
            Vec3d dir = Vec3d(v4.x, v4.y, v4.z) / v4.w;
            double denom = glm::dot(n, dir);
            if (std::abs(denom) < 1e-12) continue;
            double s = view.planeH / denom;
            if (s <= 0.0) continue;
            Vec3d pos = dir * s;
            glm::dvec4 clip = proj * glm::dvec4(pos, 1.0);
            double d = clip.z / clip.w;
            if (d < 0.0 || d > 1.0) continue;
            depth[static_cast<size_t>(j) * kFullW + i] = d;
        }
    }
    return depth;
}

double QuantizeD24(double d)
{
    double q = std::round(std::clamp(d, 0.0, 1.0) * 16777215.0) / 16777215.0;
    return q;
}

// Exact plane depth at an arbitrary uv (no texel grid, no storage
// quantization). Used as the control sampler: with continuous depth the
// reconstruction must return the plane normal up to rounding.
double AnalyticDepthAt(glm::dvec2 uv, const ViewSetup& view, const Mat4d& proj, const Mat4d& invProj)
{
    Vec3d n(view.trueNormalView.x, view.trueNormalView.y, view.trueNormalView.z);
    glm::dvec2 ndc = uv * 2.0 - 1.0;
    glm::dvec4 v4 = invProj * glm::dvec4(ndc.x, -ndc.y, 1.0, 1.0);
    Vec3d dir = Vec3d(v4.x, v4.y, v4.z) / v4.w;
    double denom = glm::dot(n, dir);
    if (std::abs(denom) < 1e-12) return 1.0;
    double s = view.planeH / denom;
    if (s <= 0.0) return 1.0;
    glm::dvec4 clip = proj * glm::dvec4(dir * s, 1.0);
    double d = clip.z / clip.w;
    if (d < 0.0 || d > 1.0) return 1.0;
    return d;
}

// ---------------------------------------------------------------------------
// One measured execution of the SSAO normal reconstruction over a grid.
// ---------------------------------------------------------------------------

struct Metrics {
    int valid = 0;
    int sky = 0;
    int edge = 0;
    int dropout = 0;
    double meanErrDeg = 0.0;
    double p95ErrDeg = 0.0;
    double maxErrDeg = 0.0;
    double adjXDeg = 0.0;   // mean angle between horizontal neighbours
    double adjYDeg = 0.0;   // mean angle between vertical neighbours
    double adjXP95Deg = 0.0;
    double stripeXDeg = 0.0; // |mean err(even columns) - mean err(odd columns)|
    double stripeYDeg = 0.0; // |mean err(even rows) - mean err(odd rows)|
};

double AngleDeg(double absDot)
{
    return std::acos(std::clamp(absDot, 0.0, 1.0)) * 57.29577951308232;
}

template <typename T, typename Sampler>
Metrics RunGrid(const Sampler& sampleDepth, const Mat4<T>& invProj, Vec3<T> trueNormal,
                int gridW, int gridH, std::vector<double>* rowMeanErr = nullptr)
{
    using Vec2 = glm::vec<2, T, glm::defaultp>;
    // ssao.slang fragmentMain: depthTexelSize = 1 / (uScreenSize * 2) with
    // uScreenSize set by the backends to the half-res target size, i.e.
    // one full-res depth texel. The same stencil is used for the full-res
    // grid so that only the execution grid changes between the two runs.
    Vec2 texelSize(T(1) / T(kFullW), T(1) / T(kFullH));
    std::vector<double> err(static_cast<size_t>(gridW) * gridH, -1.0);
    std::vector<Vec3<T>> normals(err.size());
    std::vector<char> hasNormal(err.size(), 0);
    Metrics m;
    std::vector<double> sorted;
    sorted.reserve(err.size());
    double sum = 0.0;
    double sumEvenX = 0.0, sumOddX = 0.0;
    int nEvenX = 0, nOddX = 0;
    double sumEvenY = 0.0, sumOddY = 0.0;
    int nEvenY = 0, nOddY = 0;

    for (int y = 0; y < gridH; ++y) {
        for (int x = 0; x < gridW; ++x) {
            Vec2 uv(T((x + 0.5) / gridW), T((y + 0.5) / gridH));
            Vec2 depthUv = SnapDepthUV(uv);
            T centerDepth = sampleDepth(depthUv);
            size_t idx = static_cast<size_t>(y) * gridW + x;
            if (centerDepth >= T(0.9999)) { ++m.sky; continue; } // shader sky early-out
            // Error statistics cover texels whose whole stencil lies on
            // the surface: centre and four neighbours on the surface and
            // the stencil inside the texture. Texels touching the sky or
            // the screen border are counted as edge texels instead; a
            // silhouette is not a smooth surface and says nothing about
            // striping on one.
            Vec2 uvR = SnapDepthUV(depthUv + Vec2(texelSize.x, T(0)));
            Vec2 uvL = SnapDepthUV(depthUv - Vec2(texelSize.x, T(0)));
            Vec2 uvU = SnapDepthUV(depthUv + Vec2(T(0), texelSize.y));
            Vec2 uvD = SnapDepthUV(depthUv - Vec2(T(0), texelSize.y));
            bool inside = uvL.x >= T(0) && uvR.x <= T(1) && uvD.y >= T(0) && uvU.y <= T(1);
            if (!inside || sampleDepth(uvR) >= T(0.9999) || sampleDepth(uvL) >= T(0.9999) ||
                sampleDepth(uvU) >= T(0.9999) || sampleDepth(uvD) >= T(0.9999)) {
                ++m.edge;
                continue;
            }
            Vec3<T> fragPos = ReconstructViewPos(depthUv, centerDepth, invProj);
            Vec3<T> nrm;
            if (!ReconstructNormal(depthUv, texelSize, fragPos, sampleDepth, invProj, nrm)) {
                ++m.dropout;
                continue;
            }
            double e = AngleDeg(std::abs(static_cast<double>(glm::dot(nrm, trueNormal))));
            err[idx] = e;
            normals[idx] = nrm;
            hasNormal[idx] = 1;
            ++m.valid;
            sum += e;
            sorted.push_back(e);
            if (x & 1) { sumOddX += e; ++nOddX; } else { sumEvenX += e; ++nEvenX; }
            if (y & 1) { sumOddY += e; ++nOddY; } else { sumEvenY += e; ++nEvenY; }
        }
    }
    if (m.valid > 0) {
        m.meanErrDeg = sum / m.valid;
        std::sort(sorted.begin(), sorted.end());
        m.p95ErrDeg = sorted[std::min(sorted.size() - 1,
                            static_cast<size_t>(0.95 * static_cast<double>(sorted.size())))];
        m.maxErrDeg = sorted.back();
        if (nEvenX && nOddX) m.stripeXDeg = std::abs(sumEvenX / nEvenX - sumOddX / nOddX);
        if (nEvenY && nOddY) m.stripeYDeg = std::abs(sumEvenY / nEvenY - sumOddY / nOddY);
    }
    double adjSumX = 0.0, adjSumY = 0.0;
    int adjNX = 0, adjNY = 0;
    std::vector<double> adjX;
    for (int y = 0; y < gridH; ++y) {
        for (int x = 0; x < gridW; ++x) {
            size_t idx = static_cast<size_t>(y) * gridW + x;
            if (!hasNormal[idx]) continue;
            if (x + 1 < gridW && hasNormal[idx + 1]) {
                double a = AngleDeg(std::abs(static_cast<double>(
                    glm::dot(normals[idx], normals[idx + 1]))));
                adjSumX += a; ++adjNX; adjX.push_back(a);
            }
            if (y + 1 < gridH && hasNormal[idx + gridW]) {
                double a = AngleDeg(std::abs(static_cast<double>(
                    glm::dot(normals[idx], normals[idx + gridW]))));
                adjSumY += a; ++adjNY;
            }
        }
    }
    if (rowMeanErr) {
        rowMeanErr->assign(static_cast<size_t>(gridH), -1.0);
        for (int y = 0; y < gridH; ++y) {
            double s = 0.0;
            int n = 0;
            for (int x = 0; x < gridW; ++x) {
                double e = err[static_cast<size_t>(y) * gridW + x];
                if (e >= 0.0) { s += e; ++n; }
            }
            if (n) (*rowMeanErr)[static_cast<size_t>(y)] = s / n;
        }
    }
    if (adjNX) m.adjXDeg = adjSumX / adjNX;
    if (adjNY) m.adjYDeg = adjSumY / adjNY;
    if (!adjX.empty()) {
        std::sort(adjX.begin(), adjX.end());
        m.adjXP95Deg = adjX[std::min(adjX.size() - 1,
                           static_cast<size_t>(0.95 * static_cast<double>(adjX.size())))];
    }
    return m;
}

bool CheckCase(const char* viewName, const char* gridName, const char* depthName,
               const Metrics& m, double meanLimit, double p95Limit, double adjLimit,
               double stripeLimit)
{
    std::printf("[METRIC] view=%s grid=%s depth=%s valid=%d sky=%d edge=%d dropout=%d "
                "meanErrDeg=%.4f p95ErrDeg=%.4f maxErrDeg=%.4f adjXDeg=%.4f adjXP95Deg=%.4f "
                "adjYDeg=%.4f stripeXDeg=%.4f stripeYDeg=%.4f\n",
                viewName, gridName, depthName, m.valid, m.sky, m.edge, m.dropout,
                m.meanErrDeg, m.p95ErrDeg, m.maxErrDeg, m.adjXDeg, m.adjXP95Deg,
                m.adjYDeg, m.stripeXDeg, m.stripeYDeg);
    bool ok = m.valid > 1000 && m.dropout == 0 && m.meanErrDeg <= meanLimit &&
              m.p95ErrDeg <= p95Limit && m.adjXDeg <= adjLimit && m.adjYDeg <= adjLimit &&
              m.stripeXDeg <= stripeLimit && m.stripeYDeg <= stripeLimit;
    std::printf("[%s] view=%s grid=%s depth=%s (limits mean<=%.3f p95<=%.3f adj<=%.3f stripe<=%.3f deg)\n",
                ok ? "PASS" : "FAIL", viewName, gridName, depthName,
                meanLimit, p95Limit, adjLimit, stripeLimit);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const bool profile = argc > 1 && std::string(argv[1]) == "--profile";
    const float aspect = static_cast<float>(kFullW) / static_cast<float>(kFullH);
    Mat4f projF = glm::perspectiveRH_ZO(glm::radians(kFovDeg), aspect, kNear, kFar);
    Mat4f invProjF = glm::inverse(projF); // backends upload glm::inverse(projection)
    Mat4d projD = glm::perspectiveRH_ZO<double>(glm::radians(static_cast<double>(kFovDeg)),
                                                static_cast<double>(aspect),
                                                static_cast<double>(kNear),
                                                static_cast<double>(kFar));
    Mat4d invProjD = glm::inverse(projD);

    // FPS eye-level view: camera 1.7 m above the ground, pitched down
    // about 3 degrees. Editor-style orbit view: camera raised and pulled
    // back, looking down at the ground at about 45 degrees.
    ViewSetup views[2] = {
        MakeView("grazing", Vec3f(0.0f, 1.7f, 0.0f), Vec3f(0.0f, 1.18f, -10.0f)),
        MakeView("orbit",   Vec3f(0.0f, 11.0f, 11.0f), Vec3f(0.0f, 0.0f, 0.0f)),
    };

    bool ok = true;
    for (const ViewSetup& view : views) {
        std::vector<double> analytic = RenderAnalyticDepth(view, projD, invProjD);
        std::vector<float> depthF32(analytic.size());
        std::vector<float> depthD24(analytic.size());
        for (size_t i = 0; i < analytic.size(); ++i) {
            depthF32[i] = static_cast<float>(analytic[i]);       // VK_FORMAT_D32_SFLOAT
            depthD24[i] = static_cast<float>(QuantizeD24(analytic[i])); // D24_UNORM_S8_UINT
        }
        Vec3f nF = view.trueNormalView;
        Vec3d nD(nF.x, nF.y, nF.z);

        struct GridCase { const char* name; int w; int h; };
        const GridCase grids[2] = {{"half", kFullW / 2, kFullH / 2}, {"full", kFullW, kFullH}};
        auto sampleF32 = [&depthF32](glm::vec2 uv) {
            return SampleDepthPoint(depthF32, kFullW, kFullH, uv);
        };
        auto sampleD24 = [&depthD24](glm::vec2 uv) {
            return SampleDepthPoint(depthD24, kFullW, kFullH, uv);
        };
        auto sampleExact = [&view, &projD, &invProjD](glm::dvec2 uv) {
            return AnalyticDepthAt(uv, view, projD, invProjD);
        };
        for (const GridCase& g : grids) {
            // Stored-depth executions use the float shader replica exactly.
            // Correct behaviour on a smooth plane: sub-degree error and no
            // texel-to-texel striping at either execution resolution.
            Metrics mf = RunGrid<float>(sampleF32, invProjF, nF, g.w, g.h);
            ok &= CheckCase(view.name, g.name, "f32", mf, 0.5, 2.0, 0.5, 0.25);
            Metrics mq = RunGrid<float>(sampleD24, invProjF, nF, g.w, g.h);
            ok &= CheckCase(view.name, g.name, "d24", mq, 0.5, 2.0, 0.5, 0.25);
            // Continuous-depth double control: validates the replica itself.
            std::vector<double> rowMean;
            Metrics mc = RunGrid<double>(sampleExact, invProjD, nD, g.w, g.h,
                                         profile ? &rowMean : nullptr);
            ok &= CheckCase(view.name, g.name, "f64-control", mc, 0.05, 0.25, 0.05, 0.025);
            if (profile) {
                for (int y = 0; y < g.h; y += std::max(1, g.h / 18)) {
                    if (rowMean[static_cast<size_t>(y)] >= 0.0)
                        std::printf("[ROW] view=%s grid=%s row=%d meanErrDeg=%.4f\n",
                                    view.name, g.name, y, rowMean[static_cast<size_t>(y)]);
                }
            }
        }
    }

    std::printf("%s\n", ok ? "all ssao normal tests passed" : "ssao normal tests FAILED");
    return ok ? 0 : 1;
}
