// Regression tests for screen-space LOD selection
// (Engine/src/Graphics/LODSelector.hpp, used by renderer.hpp call sites
// that build thresholds[0]=unused, thresholds[i+1]=lod_levels[i].screen_threshold).
// Run: LODTests.exe - exit code 0 means all tests passed.
//
// Contract derived from LODSelector.hpp:
//  - lod_count <= 1 returns 0 without reading screen_thresholds.
//  - Otherwise coverage = 2*radius*abs(proj[1][1])/distance, radius from
//    AABB extent*abs(scale), 1.0 when distance<=radius, 0 when radius<=0.
//  - Levels are probed coarsest-first: for i=lod_count-1..1, the first
//    coverage <= screen_thresholds[i] wins. Boundaries are inclusive and
//    screen_thresholds[0] is never read. No state is kept between calls.

#include "Graphics/LODSelector.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

static bool approx(float a, float b, float eps = 1e-6f) { return std::abs(a - b) <= eps; }
static bool fail(const std::string& n, const std::string& r) { std::cerr << "[FAIL] " << n << ": " << r << std::endl; return false; }
static bool pass(const std::string& n) { std::cout << "[PASS] " << n << std::endl; return true; }

// AABB (0,0,0)-(2,0,0): radius exactly 1, local centre (1,0,0).
// Placing the object at (dist-1,0,0) puts its centre dist from a camera
// at the origin, so with proj[1][1]==1 coverage is exactly 2/dist.
static glm::mat4 projWith(float m11) { glm::mat4 p(1.0f); p[1][1] = m11; return p; }
static int selectAt(float dist, int lodCount, const float* thr, glm::vec3 scale = glm::vec3(1.0f), glm::mat4 proj = projWith(1.0f))
{
    return LODSelector::selectLOD(glm::vec3(0.0f), glm::vec3(dist - 1.0f, 0.0f, 0.0f),
                                  glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), proj, lodCount, thr, scale);
}
static float coverageAt(float dist, glm::vec3 scale = glm::vec3(1.0f), glm::mat4 proj = projWith(1.0f))
{
    return LODSelector::computeScreenCoverage(glm::vec3(0.0f), glm::vec3(dist - 1.0f, 0.0f, 0.0f),
                                              glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), proj, scale);
}

static bool testZeroSingleNegativeCount()
{
    const std::string n = "zero, negative and single LOD counts return 0 without reading thresholds";
    // Null thresholds prove the early return never dereferences them.
    if (selectAt(100.0f, 0, nullptr) != 0) return fail(n, "lod_count=0");
    if (selectAt(100.0f, -3, nullptr) != 0) return fail(n, "lod_count<0");
    if (selectAt(100.0f, 1, nullptr) != 0) return fail(n, "lod_count=1 far");
    if (selectAt(0.5f, 1, nullptr) != 0) return fail(n, "lod_count=1 near");
    return pass(n);
}

static bool testExactThresholds()
{
    const std::string n = "distance exactly at each threshold selects that threshold's LOD (inclusive)";
    // coverage 2/dist: dist 4 -> 0.5, 8 -> 0.25, 16 -> 0.125, all exact in float.
    float thr3[] = {0.0f, 0.5f, 0.25f};
    if (!approx(coverageAt(4.0f), 0.5f) || !approx(coverageAt(8.0f), 0.25f)) return fail(n, "coverage setup");
    if (selectAt(4.0f, 3, thr3) != 1) return fail(n, "coverage==0.5 must give LOD1");
    if (selectAt(8.0f, 3, thr3) != 2) return fail(n, "coverage==0.25 must give LOD2");
    float thr4[] = {0.0f, 0.5f, 0.25f, 0.125f};
    if (selectAt(4.0f, 4, thr4) != 1) return fail(n, "4-level at 0.5");
    if (selectAt(8.0f, 4, thr4) != 2) return fail(n, "4-level at 0.25");
    if (selectAt(16.0f, 4, thr4) != 3) return fail(n, "4-level at 0.125");
    if (selectAt(2.0f, 4, thr4) != 0) return fail(n, "coverage 1.0 above all thresholds must give LOD0");
    return pass(n);
}

static bool testJustEitherSideOfThresholds()
{
    const std::string n = "coverage just above/below each threshold flips exactly one level";
    float thr[] = {0.0f, 0.5f, 0.25f};
    if (selectAt(3.9f, 3, thr) != 0 || selectAt(4.1f, 3, thr) != 1) return fail(n, "around 0.5");
    if (selectAt(7.9f, 3, thr) != 1 || selectAt(8.1f, 3, thr) != 2) return fail(n, "around 0.25");
    // Monotonic non-decreasing LOD as distance grows over a sweep.
    int prev = 0;
    for (float d = 1.5f; d <= 64.0f; d *= 1.13f) { int lod = selectAt(d, 3, thr); if (lod < prev) return fail(n, "non-monotonic sweep"); prev = lod; }
    if (prev != 2) return fail(n, "sweep must end at coarsest");
    return pass(n);
}

static bool testNoHysteresisPopsAtBoundary()
{
    const std::string n = "no hysteresis: oscillating across a threshold toggles LOD every call";
    float thr[] = {0.0f, 0.5f, 0.25f};
    // Stateless selection means a camera hovering at a boundary pops
    // between levels on successive frames; this pins that behaviour.
    for (int k = 0; k < 8; ++k)
    {
        if (selectAt(3.9f, 3, thr) != 0) return fail(n, "above threshold must be LOD0 every time");
        if (selectAt(4.1f, 3, thr) != 1) return fail(n, "below threshold must be LOD1 every time");
    }
    return pass(n);
}

static bool testFarEntity()
{
    const std::string n = "far entities select the coarsest LOD";
    float thr[] = {0.0f, 0.5f, 0.25f};
    if (selectAt(1000.0f, 3, thr) != 2) return fail(n, "dist 1000");
    if (selectAt(1.0e6f, 3, thr) != 2) return fail(n, "dist 1e6");
    if (selectAt(1.0e18f, 3, thr) != 2) return fail(n, "dist 1e18, coverage ~2e-18");
    if (!approx(coverageAt(1000.0f), 0.002f, 1e-6f)) return fail(n, "far coverage value");
    return pass(n);
}

static bool testCameraInsideAndNearCoverage()
{
    const std::string n = "camera inside bounding sphere caps coverage at 1 and selects LOD0";
    float thr[] = {0.0f, 0.5f, 0.25f};
    if (!approx(coverageAt(0.0f), 1.0f) || !approx(coverageAt(0.5f), 1.0f) || !approx(coverageAt(1.0f), 1.0f))
        return fail(n, "distance<=radius must return 1");
    if (selectAt(0.5f, 3, thr) != 0) return fail(n, "inside must be LOD0 for thresholds < 1");
    // Just outside: coverage exceeds 1 (documented 0..1+ range), still LOD0.
    if (!approx(coverageAt(1.5f), 2.0f / 1.5f, 1e-5f)) return fail(n, "near coverage >1");
    if (selectAt(1.5f, 3, thr) != 0) return fail(n, "coverage >1 must be LOD0");
    return pass(n);
}

static bool testDegenerateAABB()
{
    const std::string n = "degenerate point AABB has zero coverage and selects the coarsest LOD";
    float thr[] = {0.0f, 0.5f, 0.25f};
    float cov = LODSelector::computeScreenCoverage(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f),
                                                   glm::vec3(1.0f), glm::vec3(1.0f), projWith(1.0f));
    if (!approx(cov, 0.0f)) return fail(n, "point radius must give coverage 0");
    int lod = LODSelector::selectLOD(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f),
                                     glm::vec3(1.0f), glm::vec3(1.0f), projWith(1.0f), 3, thr);
    if (lod != 2) return fail(n, "coverage 0 <= smallest positive threshold must give coarsest");
    // Inverted AABB (min>max): extent is negative, length() keeps radius 1.
    float covInv = LODSelector::computeScreenCoverage(glm::vec3(0.0f), glm::vec3(3.0f, 0.0f, 0.0f),
                                                      glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), projWith(1.0f));
    if (!approx(covInv, coverageAt(4.0f))) return fail(n, "inverted AABB must match normal AABB");
    return pass(n);
}

static bool testThresholdZeroAndNegative()
{
    const std::string n = "zero or negative thresholds keep even far entities at LOD0";
    float zero[] = {0.0f, 0.0f, 0.0f};
    // Any positive coverage fails coverage<=0, so coarsest is unreachable.
    if (selectAt(1000.0f, 3, zero) != 0) return fail(n, "zero thresholds, positive coverage");
    float neg[] = {0.0f, -0.5f, -0.25f};
    if (selectAt(1000.0f, 3, neg) != 0) return fail(n, "negative thresholds");
    // Exactly zero coverage (point AABB) does satisfy 0<=0, inclusively.
    int lod = LODSelector::selectLOD(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f),
                                     glm::vec3(1.0f), glm::vec3(1.0f), projWith(1.0f), 3, zero);
    if (lod != 2) return fail(n, "zero coverage with zero threshold must give coarsest");
    return pass(n);
}

static bool testThresholdOrderingAssumptions()
{
    const std::string n = "non-decreasing or equal thresholds make intermediate LODs unreachable";
    // Coarsest-first probe: with thresholds rising, LOD2 wins wherever
    // coverage<=0.5 and LOD1 is never returned at any distance.
    float rising[] = {0.0f, 0.25f, 0.5f};
    for (float d = 1.5f; d <= 64.0f; d *= 1.21f)
        if (selectAt(d, 3, rising) == 1) return fail(n, "rising thresholds must never yield LOD1");
    if (selectAt(8.0f, 3, rising) != 2) return fail(n, "coverage 0.25 must yield LOD2 under rising thresholds");
    if (selectAt(3.0f, 3, rising) != 0) return fail(n, "coverage 0.667 must yield LOD0 under rising thresholds");
    // Equal thresholds: the coarser level is checked first, so LOD1 is shadowed.
    float equal[] = {0.0f, 0.5f, 0.5f};
    if (selectAt(4.0f, 3, equal) != 2) return fail(n, "equal thresholds at boundary must give coarsest");
    for (float d = 1.5f; d <= 64.0f; d *= 1.21f)
        if (selectAt(d, 3, equal) == 1) return fail(n, "equal thresholds must never yield LOD1");
    return pass(n);
}

static bool testThresholdZeroIndexIgnored()
{
    const std::string n = "screen_thresholds[0] is ignored";
    float a[] = {999.0f, 0.5f, 0.25f};
    float b[] = {-999.0f, 0.5f, 0.25f};
    for (float d : {2.0f, 4.0f, 8.0f, 100.0f})
        if (selectAt(d, 3, a) != selectAt(d, 3, b)) return fail(n, "index 0 changed selection");
    return pass(n);
}

static bool testScaleAndProjection()
{
    const std::string n = "scale and projection scale move thresholds predictably (abs applied)";
    float thr[] = {0.0f, 0.5f, 0.25f};
    // Scale 2: radius 2, centre offset 2, so centre distance is dist+1
    // when object sits at dist-1; use coverage helper geometry instead:
    // at object position giving centre distance 8, coverage = 4/8 = 0.5.
    float covScaled = LODSelector::computeScreenCoverage(glm::vec3(0.0f), glm::vec3(6.0f, 0.0f, 0.0f),
                                                         glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), projWith(1.0f), glm::vec3(2.0f));
    if (!approx(covScaled, 0.5f)) return fail(n, "scale 2 coverage");
    // Negative scale: extent uses abs, centre flips sign; mirrored centre
    // at the same distance must give identical coverage.
    float covNeg = LODSelector::computeScreenCoverage(glm::vec3(0.0f), glm::vec3(5.0f, 0.0f, 0.0f),
                                                      glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), projWith(1.0f), glm::vec3(-1.0f));
    if (!approx(covNeg, 0.5f)) return fail(n, "negative scale coverage (centre at 4, radius 1)");
    // Zero scale collapses radius to 0 -> coverage 0 -> coarsest.
    if (selectAt(4.0f, 3, thr, glm::vec3(0.0f)) != 2) return fail(n, "zero scale");
    // Projection: m11=2 doubles coverage, m11=-2 same via abs, m11=0 zeroes it.
    if (!approx(coverageAt(8.0f, glm::vec3(1.0f), projWith(2.0f)), 0.5f)) return fail(n, "proj 2");
    if (!approx(coverageAt(8.0f, glm::vec3(1.0f), projWith(-2.0f)), 0.5f)) return fail(n, "proj -2 abs");
    if (selectAt(8.0f, 3, thr, glm::vec3(1.0f), projWith(2.0f)) != 1) return fail(n, "proj 2 selection at boundary");
    if (selectAt(4.0f, 3, thr, glm::vec3(1.0f), projWith(0.0f)) != 2) return fail(n, "proj 0 must give coarsest");
    return pass(n);
}

static bool testManyLevelsAndThresholdAtOne()
{
    const std::string n = "many levels walk to the correct rung; threshold >=1 captures camera-inside case";
    float thr[] = {0.0f, 0.8f, 0.6f, 0.4f, 0.2f, 0.1f, 0.05f, 0.025f};
    // coverage: dist 10 -> 0.2 (exact rung 4), 40 -> 0.05 (rung 6), 80 -> 0.025 (rung 7)
    if (selectAt(10.0f, 8, thr) != 4) return fail(n, "dist 10");
    if (selectAt(40.0f, 8, thr) != 6) return fail(n, "dist 40");
    if (selectAt(80.0f, 8, thr) != 7) return fail(n, "dist 80");
    if (selectAt(2.5f, 8, thr) != 1) return fail(n, "coverage 0.8 boundary must give LOD1");
    // Threshold of exactly 1.0 is inclusive even for the inside-sphere cap.
    float one[] = {0.0f, 1.0f, 1.0f};
    if (selectAt(0.5f, 3, one) != 2) return fail(n, "coverage 1.0 <= threshold 1.0 must give coarsest");
    return pass(n);
}

static bool testNaNCoverageFallsToFinest()
{
    const std::string n = "NaN coverage satisfies no threshold and falls through to LOD0";
    float thr[] = {0.0f, 0.5f, 0.25f};
    float nan = std::numeric_limits<float>::quiet_NaN();
    int lod = LODSelector::selectLOD(glm::vec3(nan, 0.0f, 0.0f), glm::vec3(10.0f, 0.0f, 0.0f),
                                     glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), projWith(1.0f), 3, thr);
    if (lod != 0) return fail(n, "NaN comparisons are false, expected LOD0");
    return pass(n);
}

int main()
{
    bool ok = true;
    ok &= testZeroSingleNegativeCount();
    ok &= testExactThresholds();
    ok &= testJustEitherSideOfThresholds();
    ok &= testNoHysteresisPopsAtBoundary();
    ok &= testFarEntity();
    ok &= testCameraInsideAndNearCoverage();
    ok &= testDegenerateAABB();
    ok &= testThresholdZeroAndNegative();
    ok &= testThresholdOrderingAssumptions();
    ok &= testThresholdZeroIndexIgnored();
    ok &= testScaleAndProjection();
    ok &= testManyLevelsAndThresholdAtOne();
    ok &= testNaNCoverageFallsToFinest();
    std::cout << (ok ? "all LOD tests passed" : "LOD tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
