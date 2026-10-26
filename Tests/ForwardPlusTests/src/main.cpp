// Regression tests for Forward+ light binning
// (Engine/src/Graphics/ForwardPlusLightBinning.hpp, used by the
// Vulkan/D3D12/Metal backends). Run: ForwardPlusTests.exe — exit code 0
// means all tests passed.

#include "Graphics/ForwardPlusLightBinning.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <set>
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

// Camera at the origin looking down -Z (right-handed, y-up), 90 degree fov,
// square 160x160 viewport: 10x10 tiles of 16 px, screen centre is tile (5,5).
struct TestCamera
{
    glm::mat4 view{1.0f};
    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    uint32_t width = 160;
    uint32_t height = 160;
};

static GPUPointLight makePoint(float x, float y, float z, float range)
{
    GPUPointLight light{};
    light.position = glm::vec3(x, y, z);
    light.range = range;
    return light;
}

static const ForwardPlusTile& cellAt(const ForwardPlusBinningResult& r, uint32_t slice, uint32_t tx, uint32_t ty)
{
    return r.tiles[1u + (slice * r.tilesY + ty) * r.tilesX + tx];
}

static std::set<uint32_t> indicesInCell(const ForwardPlusBinningResult& r, const ForwardPlusTile& cell)
{
    std::set<uint32_t> out;
    for (uint32_t i = 0; i < cell.lightCount; ++i)
        out.insert(r.lightIndices[cell.lightOffset + i]);
    return out;
}

// All slice indices in which `lightIndex` appears anywhere in the grid.
static std::set<uint32_t> slicesContaining(const ForwardPlusBinningResult& r, uint32_t lightIndex)
{
    std::set<uint32_t> out;
    const uint32_t tileCount = r.tilesX * r.tilesY;
    for (uint32_t cell = 0; cell + 1u < r.tiles.size(); ++cell)
    {
        const ForwardPlusTile& tile = r.tiles[1u + cell];
        for (uint32_t i = 0; i < tile.lightCount; ++i)
            if (r.lightIndices[tile.lightOffset + i] == lightIndex)
                out.insert(cell / tileCount);
    }
    return out;
}

static bool testLightLandsInCentreTile()
{
    const std::string name = "light in front of camera lands in the centre tile";
    TestCamera cam;
    GPUPointLight lights[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult r = binForwardPlusLights(lights, 1, nullptr, 0, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (r.tilesX != 10 || r.tilesY != 10)
        return fail(name, "expected a 10x10 tile grid");
    const ForwardPlusTile& centre = cellAt(r, 0, 5, 5);
    if (centre.lightCount == 0 || !indicesInCell(r, centre).count(0u))
        return fail(name, "centre tile (5,5) does not contain light 0");
    // flipY: a light above centre projects to the upper half (tile y < 5)
    // with flipY=true and to the lower half with flipY=false.
    GPUPointLight above[] = {makePoint(0.0f, 1.0f, -5.0f, 0.1f)};
    ForwardPlusBinningResult flipped = binForwardPlusLights(above, 1, nullptr, 0, cam.view, cam.proj,
                                                            cam.width, cam.height, 0.1f, 1000.0f, true, true);
    ForwardPlusBinningResult unflipped = binForwardPlusLights(above, 1, nullptr, 0, cam.view, cam.proj,
                                                              cam.width, cam.height, 0.1f, 1000.0f, false, true);
    bool foundFlipped = false, foundUnflipped = false;
    for (uint32_t ty = 0; ty < 5; ++ty)
        if (cellAt(flipped, 0, 5, ty).lightCount > 0) foundFlipped = true;
    for (uint32_t ty = 5; ty < 10; ++ty)
        if (cellAt(unflipped, 0, 5, ty).lightCount > 0) foundUnflipped = true;
    if (!foundFlipped || !foundUnflipped)
        return fail(name, "flipY does not mirror the tile row of an above-centre light");
    return pass(name);
}

static bool testBehindAndOffScreenExcluded()
{
    const std::string name = "behind-camera / off-screen lights excluded, camera-enclosing light full-screen";
    TestCamera cam;
    GPUPointLight lights[] = {makePoint(0.0f, 0.0f, 5.0f, 0.5f),    // behind
                              makePoint(100.0f, 0.0f, -5.0f, 1.0f)}; // off-screen
    ForwardPlusBinningResult r = binForwardPlusLights(lights, 2, nullptr, 0, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (!r.lightIndices.empty())
        return fail(name, "behind/off-screen lights were binned");

    GPUPointLight enclosing[] = {makePoint(0.0f, 0.0f, -0.5f, 2.0f)};
    ForwardPlusBinningResult full = binForwardPlusLights(enclosing, 1, nullptr, 0, cam.view, cam.proj,
                                                         cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (cellAt(full, 0, 0, 0).lightCount == 0 || cellAt(full, 0, 9, 9).lightCount == 0)
        return fail(name, "camera-enclosing light does not cover corner tiles");
    return pass(name);
}

static bool testSpotIndexOffsetByPointCount()
{
    const std::string name = "spot light index is offset by the point light count";
    TestCamera cam;
    GPUPointLight points[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    GPUSpotLight spots[1]{};
    spots[0].position = glm::vec3(0.0f, 0.0f, -5.0f);
    spots[0].range = 0.5f;
    ForwardPlusBinningResult r = binForwardPlusLights(points, 1, spots, 1, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    std::set<uint32_t> idx = indicesInCell(r, cellAt(r, 0, 5, 5));
    if (!idx.count(0u) || !idx.count(1u))
        return fail(name, "centre tile does not contain point index 0 and spot index 1");
    return pass(name);
}

static bool testPerTileCap()
{
    const std::string name = "per-tile light cap of 64 is respected with 100 overlapping lights";
    TestCamera cam;
    std::vector<GPUPointLight> lights(100, makePoint(0.0f, 0.0f, -5.0f, 0.5f));
    ForwardPlusBinningResult r = binForwardPlusLights(lights.data(), 100, nullptr, 0, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    uint32_t maxCount = 0;
    for (size_t i = 1; i < r.tiles.size(); ++i)
        maxCount = std::max(maxCount, r.tiles[i].lightCount);
    if (maxCount != kForwardPlusMaxLightsPerTile)
        return fail(name, "expected some cell capped at exactly 64, max is " + std::to_string(maxCount));
    return pass(name);
}

static ForwardPlusBinningResult binNearFarLights()
{
    TestCamera cam;
    std::vector<GPUPointLight> lights;
    for (int i = 0; i < 8; ++i) lights.push_back(makePoint(0.0f, 0.0f, -1.0f, 0.1f));   // indices 0..7
    for (int i = 0; i < 8; ++i) lights.push_back(makePoint(0.0f, 0.0f, -100.0f, 1.0f)); // indices 8..15
    return binForwardPlusLights(lights.data(), static_cast<uint32_t>(lights.size()), nullptr, 0,
                                cam.view, cam.proj, cam.width, cam.height, 0.1f, 1000.0f, true, true);
}

static bool testZSlicing()
{
    const std::string name = "z slicing separates near lights (early slices) from far lights (late slices)";
    ForwardPlusBinningResult r = binNearFarLights();
    if (r.sliceCount <= 1)
        return fail(name, "sliceCount is 1 for 16 depth-spread lights");
    std::set<uint32_t> nearSlices = slicesContaining(r, 0u);
    std::set<uint32_t> farSlices = slicesContaining(r, 8u);
    if (nearSlices.empty() || farSlices.empty())
        return fail(name, "near or far light missing from the grid");
    if (*nearSlices.rbegin() > 1u)
        return fail(name, "near light leaks into late slices");
    if (*farSlices.begin() < 10u)
        return fail(name, "far light leaks into early slices");
    return pass(name);
}

static bool testDisabledMinimalResult()
{
    const std::string name = "disabled binning returns a valid minimal grid";
    TestCamera cam;
    GPUPointLight lights[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult r = binForwardPlusLights(lights, 1, nullptr, 0, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, false);
    if (r.sliceCount != 1 || r.tilesX != 10 || r.tilesY != 10)
        return fail(name, "minimal result has wrong grid dimensions");
    if (r.tiles.size() != 1u + 100u || r.tiles[0].lightOffset != 1u)
        return fail(name, "minimal result header/grid malformed");
    if (r.tiles[1].lightOffset != 0 || r.tiles[1].lightCount != 0 || !r.lightIndices.empty())
        return fail(name, "minimal result is not empty");
    return pass(name);
}

static bool testGridHeaderRoundTrip()
{
    const std::string name = "grid header encodes slice params that round-trip through the shader slice math";
    ForwardPlusBinningResult r = binNearFarLights();
    const ForwardPlusTile& header = r.tiles[0];
    if (header.lightOffset != r.sliceCount || header.lightOffset <= 1u)
        return fail(name, "header sliceCount mismatch");

    // Rebuild the grid from the header exactly as the shader does
    // (common.slang forwardPlusGridSlice): scale/offset/distribution are the
    // bitcast floats in lightCount/padding0/padding1.
    ForwardPlusGridZ grid;
    grid.sliceCount = header.lightOffset;
    grid.scale = std::bit_cast<float>(header.lightCount);
    grid.offset = std::bit_cast<float>(header.padding0);
    grid.distribution = std::bit_cast<float>(header.padding1);

    auto shaderSlice = [&](float viewDepth) {
        float term = viewDepth * grid.scale + grid.offset;
        float s = std::log2(std::max(term, 1.0f)) * grid.distribution;
        return std::min(static_cast<uint32_t>(std::max(s, 0.0f)), grid.sliceCount - 1u);
    };
    for (float depth : {0.2f, 1.0f, 3.0f, 10.0f, 30.0f, 100.0f, 500.0f})
    {
        if (shaderSlice(depth) != forwardPlusGridSlice(grid, depth))
            return fail(name, "shader-mirror slice disagrees with forwardPlusGridSlice");
    }
    if (shaderSlice(1.0f) != 0u)
        return fail(name, "near depth is not in slice 0");
    if (shaderSlice(100.0f) < 10u)
        return fail(name, "far depth is not in a late slice");
    // The C++ grid for the same near/far must produce identical parameters.
    ForwardPlusGridZ direct = makeForwardPlusGridZ(0.1f, 100.0f, 16);
    if (direct.sliceCount <= 1u || forwardPlusGridSlice(direct, 1.0f) != 0u ||
        forwardPlusGridSlice(direct, 100.0f) < 10u)
        return fail(name, "makeForwardPlusGridZ slice placement is wrong");
    return pass(name);
}

static bool sameBinning(const ForwardPlusBinningResult& a, const ForwardPlusBinningResult& b)
{
    if (a.tilesX != b.tilesX || a.tilesY != b.tilesY || a.sliceCount != b.sliceCount)
        return false;
    if (a.tiles.size() != b.tiles.size() || a.lightIndices != b.lightIndices)
        return false;
    for (size_t i = 0; i < a.tiles.size(); ++i)
        if (a.tiles[i].lightOffset != b.tiles[i].lightOffset ||
            a.tiles[i].lightCount != b.tiles[i].lightCount ||
            a.tiles[i].padding0 != b.tiles[i].padding0 ||
            a.tiles[i].padding1 != b.tiles[i].padding1)
            return false;
    return true;
}

static bool testReusedScratchMatchesFreshAndKeepsCapacity()
{
    const std::string name = "reused binning scratch matches fresh output and keeps capacity";
    TestCamera cam;
    std::vector<GPUPointLight> lights;
    for (int i = 0; i < 16; ++i)
        lights.push_back(makePoint((float)(i % 4) - 1.5f, (float)((i / 4) % 4) - 1.5f,
                                   -2.0f - (float)(i * 3), 1.5f));
    ForwardPlusBinningResult fresh = binForwardPlusLights(
        lights.data(), (uint32_t)lights.size(), nullptr, 0, cam.view, cam.proj,
        cam.width, cam.height, 0.1f, 1000.0f, true, true);

    ForwardPlusBinningResult reused;
    ForwardPlusBinningScratch scratch;
    binForwardPlusLightsInto(reused, scratch, lights.data(), (uint32_t)lights.size(),
                             nullptr, 0, cam.view, cam.proj, cam.width, cam.height,
                             0.1f, 1000.0f, true, true);
    if (!sameBinning(reused, fresh))
        return fail(name, "first reused-scratch result differs from fresh result");

    const size_t tilesCap = reused.tiles.capacity();
    const size_t indicesCap = reused.lightIndices.capacity();
    const size_t binnedCap = scratch.binned.capacity();
    const size_t countsCap = scratch.cellCounts.capacity();
    const size_t offsetsCap = scratch.writeOffsets.capacity();
    if (tilesCap == 0 || binnedCap == 0 || countsCap == 0 || offsetsCap == 0)
        return fail(name, "scratch or result storage was not retained");

    binForwardPlusLightsInto(reused, scratch, lights.data(), (uint32_t)lights.size(),
                             nullptr, 0, cam.view, cam.proj, cam.width, cam.height,
                             0.1f, 1000.0f, true, true);
    if (!sameBinning(reused, fresh))
        return fail(name, "second reused-scratch result differs from fresh result");
    if (reused.tiles.capacity() != tilesCap || reused.lightIndices.capacity() != indicesCap ||
        scratch.binned.capacity() != binnedCap || scratch.cellCounts.capacity() != countsCap ||
        scratch.writeOffsets.capacity() != offsetsCap)
        return fail(name, "reused binning grew retained storage on an identical call");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testReusedScratchMatchesFreshAndKeepsCapacity();
    ok &= testLightLandsInCentreTile();
    ok &= testBehindAndOffScreenExcluded();
    ok &= testSpotIndexOffsetByPointCount();
    ok &= testPerTileCap();
    ok &= testZSlicing();
    ok &= testDisabledMinimalResult();
    ok &= testGridHeaderRoundTrip();
    std::cout << (ok ? "all Forward+ tests passed" : "Forward+ tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
