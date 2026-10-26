// Edge-case tests for Forward+ light binning
// (Engine/src/Graphics/ForwardPlusLightBinning.hpp). The happy path lives in
// ForwardPlusTests; this suite pins truncation counts, tile/slice boundary
// ownership, the unified point/spot index space, minimal grids, degenerate
// lights, and retained-scratch equivalence. Run: BinningEdgeTests.exe --
// exit code 0 means all tests passed.

#include "Graphics/ForwardPlusLightBinning.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
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

// Camera at the origin looking down -Z, 90 degree fov, 160x160 viewport:
// 10x10 tiles of 16 px, screen centre is tile (5,5).
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

static GPUSpotLight makeSpot(float x, float y, float z, float range,
                             const glm::vec3& direction = glm::vec3(0.0f, 0.0f, -1.0f))
{
    GPUSpotLight light{};
    light.position = glm::vec3(x, y, z);
    light.range = range;
    light.direction = direction;
    return light;
}

static const ForwardPlusTile& cellAt(const ForwardPlusBinningResult& r, uint32_t slice, uint32_t tx, uint32_t ty)
{
    return r.tiles[1u + slice * r.tilesX * r.tilesY + ty * r.tilesX + tx];
}

static std::vector<uint32_t> indicesInCell(const ForwardPlusBinningResult& r, const ForwardPlusTile& cell)
{
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < cell.lightCount; ++i)
        out.push_back(r.lightIndices[cell.lightOffset + i]);
    return out;
}

static bool cellContains(const ForwardPlusBinningResult& r, const ForwardPlusTile& cell, uint32_t index)
{
    std::vector<uint32_t> indices = indicesInCell(r, cell);
    return std::find(indices.begin(), indices.end(), index) != indices.end();
}

static bool anyCellContains(const ForwardPlusBinningResult& r, uint32_t index)
{
    return std::find(r.lightIndices.begin(), r.lightIndices.end(), index) != r.lightIndices.end();
}

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

static ForwardPlusGridZ gridFromHeader(const ForwardPlusBinningResult& r)
{
    ForwardPlusGridZ grid;
    grid.sliceCount = r.tiles[0].lightOffset;
    grid.scale = std::bit_cast<float>(r.tiles[0].lightCount);
    grid.offset = std::bit_cast<float>(r.tiles[0].padding0);
    grid.distribution = std::bit_cast<float>(r.tiles[0].padding1);
    return grid;
}

// A minimal result: one slice, header for the single-slice grid, every cell
// offset 0 / count 0, and no light indices.
static bool isMinimal(const ForwardPlusBinningResult& r, uint32_t tilesX, uint32_t tilesY)
{
    if (r.tilesX != tilesX || r.tilesY != tilesY || r.sliceCount != 1)
        return false;
    if (r.tiles.size() != static_cast<size_t>(tilesX) * tilesY + 1u || !r.lightIndices.empty())
        return false;
    if (r.tiles[0].lightOffset != 1u || r.tiles[0].lightCount != 0u ||
        r.tiles[0].padding0 != std::bit_cast<uint32_t>(1.0f) || r.tiles[0].padding1 != 0u)
        return false;
    for (size_t i = 1; i < r.tiles.size(); ++i)
        if (r.tiles[i].lightOffset != 0u || r.tiles[i].lightCount != 0u ||
            r.tiles[i].padding0 != 0u || r.tiles[i].padding1 != 0u)
            return false;
    return true;
}

static bool statsAreZero(const BinningStats& s)
{
    return s.lights_in == 0 && s.lights_binned == 0 && s.tiles_touched == 0 &&
           s.indices_written == 0 && s.lights_truncated == 0 && s.max_tile_occupancy == 0;
}

static bool statsEqual(const BinningStats& a, const BinningStats& b)
{
    return a.lights_in == b.lights_in && a.lights_binned == b.lights_binned &&
           a.tiles_touched == b.tiles_touched && a.indices_written == b.indices_written &&
           a.lights_truncated == b.lights_truncated && a.max_tile_occupancy == b.max_tile_occupancy;
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

static glm::mat4 orthoProj()
{
    return glm::ortho(-8.0f, 8.0f, -8.0f, 8.0f, -10.0f, 100.0f);
}

// Orthographic binning never z-slices, so counts below are single-slice
// exact. A point at the origin with range 0.5 covers screen 75..85 px in
// both axes: tiles (4..5, 4..5), four cells.
static bool checkCappedScene(const std::string& name, uint32_t pointCount, uint32_t spotCount,
                             uint64_t expectedTruncated)
{
    std::vector<GPUPointLight> points(pointCount, makePoint(0.0f, 0.0f, -5.0f, 0.5f));
    std::vector<GPUSpotLight> spots(spotCount, makeSpot(0.0f, 0.0f, -5.0f, 0.5f));
    BinningStats stats;
    ForwardPlusBinningResult r = binForwardPlusLights(
        points.empty() ? nullptr : points.data(), pointCount,
        spots.empty() ? nullptr : spots.data(), spotCount,
        glm::mat4(1.0f), orthoProj(), 160, 160, 0.1f, 1000.0f, true, true, &stats);
    if (r.sliceCount != 1)
        return fail(name, "orthographic scene unexpectedly z-sliced");
    const uint32_t total = pointCount + spotCount;
    const uint32_t capped = (std::min)(total, kForwardPlusMaxLightsPerTile);
    if (stats.lights_in != total || stats.lights_binned != total ||
        stats.indices_written != 4ull * capped || stats.lights_truncated != expectedTruncated ||
        stats.tiles_touched != 4 || stats.max_tile_occupancy != capped)
        return fail(name, "stats do not match the exact capped counts");
    if (r.lightIndices.size() != 4ull * capped)
        return fail(name, "light index buffer size is not 4 * capped count");
    std::vector<uint32_t> expectedSegment(capped);
    for (uint32_t i = 0; i < capped; ++i) expectedSegment[i] = i;
    for (uint32_t ty = 0; ty < 10; ++ty)
        for (uint32_t tx = 0; tx < 10; ++tx)
        {
            const ForwardPlusTile& cell = cellAt(r, 0, tx, ty);
            const bool touched = (tx == 4 || tx == 5) && (ty == 4 || ty == 5);
            if (!touched)
            {
                if (cell.lightCount != 0)
                    return fail(name, "untouched cell has lights");
            }
            else if (indicesInCell(r, cell) != expectedSegment)
                return fail(name, "capped cell does not hold the lowest unified indices in order");
        }
    for (uint32_t index = capped; index < total; ++index)
        if (anyCellContains(r, index))
            return fail(name, "index past the cap leaked into the grid");
    return true;
}

static bool testPerTileCapExactTruncation()
{
    const std::string name = "per-tile cap truncation count is exact (100 lights, 4 cells)";
    // 100 lights per cell, 64 kept: (100 - 64) * 4 cells = 144 truncated.
    if (!checkCappedScene(name, 100, 0, 144))
        return false;
    return pass(name);
}

static bool testCapBoundary64And65AndUnifiedSpotCrossing()
{
    const std::string name = "cap boundary 64/65 and cap crossing from points into spots";
    if (!checkCappedScene(name, 64, 0, 0))
        return false;
    // One past the cap: exactly one index dropped per touched cell.
    if (!checkCappedScene(name, 65, 0, 4))
        return false;
    // 60 points + 10 spots share one capped list per cell: unified indices
    // 0..63 survive (spots 60..63 are spot lights 0..3), 64..69 are dropped.
    // (70 - 64) * 4 cells = 24 truncated.
    if (!checkCappedScene(name, 60, 10, 24))
        return false;
    return pass(name);
}

// Identity view-projection: NDC == world xy, w == 1. A 256 px viewport
// gives a 128 px-per-unit screen scale, so the pixel coordinates used
// below are exact binary fractions in NDC and project without rounding.
static std::optional<ForwardPlusTileBounds> boundsForScreen(float centrePx, float centrePy, float radiusPx,
                                                            bool flipY)
{
    glm::vec3 position(centrePx / 128.0f - 1.0f, 0.0f, 0.0f);
    if (flipY)
        position.y = 1.0f - centrePy / 128.0f;
    else
        position.y = centrePy / 128.0f - 1.0f;
    return lightTileBounds(position, radiusPx / 128.0f, glm::mat4(1.0f), false, 256, 256, 16, 16, 16, flipY);
}

static bool testLightsExactlyOnTileBoundaries()
{
    const std::string name = "lights exactly on tile boundaries belong to the higher tile";
    // Sphere spanning screen x 32..64: the left edge is exactly the tile
    // 1/2 boundary, so minX is tile 2; the right edge at 64 is tile 4.
    auto a = boundsForScreen(48.0f, 128.0f, 16.0f, false);
    if (!a || a->minX != 2 || a->maxX != 4)
        return fail(name, "sphere starting exactly at x=32 is not in tiles 2..4");
    // Sphere spanning 0..32: max edge exactly on the boundary still reports
    // tile 2 (floor of the boundary pixel), min is tile 0.
    auto b = boundsForScreen(16.0f, 128.0f, 16.0f, false);
    if (!b || b->minX != 0 || b->maxX != 2)
        return fail(name, "sphere ending exactly at x=32 does not report tiles 0..2");
    // Y with flipY: screen y 32..64 mirrors the X case.
    auto c = boundsForScreen(128.0f, 48.0f, 16.0f, true);
    if (!c || c->minY != 2 || c->maxY != 4)
        return fail(name, "flipY sphere starting exactly at y=32 is not in tiles 2..4");
    // The same NDC y (0.625) without flip maps to screen y 192..224.
    auto e = lightTileBounds(glm::vec3(0.0f, 0.625f, 0.0f), 0.125f, glm::mat4(1.0f), false,
                             256, 256, 16, 16, 16, false);
    if (!e || e->minY != 12 || e->maxY != 14)
        return fail(name, "same NDC y without flipY does not land in tiles 12..14");
    return pass(name);
}

static bool testScreenEdgeInclusion()
{
    const std::string name = "spheres touching the screen edge from outside are included/excluded exactly";
    // Left edge exactly at x=256 (the first pixel past the viewport): out.
    if (boundsForScreen(264.0f, 128.0f, 8.0f, false))
        return fail(name, "sphere starting exactly at x=width was binned");
    // Sphere spanning 240..256, clamped to pixel 255: only tile column 15.
    auto inside = boundsForScreen(248.0f, 128.0f, 8.0f, false);
    if (!inside || inside->minX != 15 || inside->maxX != 15)
        return fail(name, "sphere ending at the right edge is not confined to tile 15");
    // Right edge exactly at x=0 touches pixel 0: in, confined to tile 0.
    auto touching = boundsForScreen(-8.0f, 128.0f, 8.0f, false);
    if (!touching || touching->minX != 0 || touching->maxX != 0)
        return fail(name, "sphere ending exactly at x=0 was not binned into tile 0");
    // Right edge at x=-8: fully outside.
    if (boundsForScreen(-24.0f, 128.0f, 16.0f, false))
        return fail(name, "sphere ending at x=-8 was binned");
    return pass(name);
}

static bool testGridZDegenerateParameters()
{
    const std::string name = "z-grid degenerate parameters collapse to a single slice";
    ForwardPlusGridZ zeroSlices = makeForwardPlusGridZ(0.1f, 100.0f, 0);
    ForwardPlusGridZ oneSlice = makeForwardPlusGridZ(0.1f, 100.0f, 1);
    if (zeroSlices.sliceCount != 1 || oneSlice.sliceCount != 1 ||
        zeroSlices.scale != 0.0f || zeroSlices.offset != 1.0f || zeroSlices.distribution != 0.0f)
        return fail(name, "fewer than two slices is not the single-slice grid");
    // Far plane leaving no range past near + 0.1 offset.
    if (makeForwardPlusGridZ(5.0f, 5.05f, 16).sliceCount != 1 ||
        makeForwardPlusGridZ(5.0f, 5.0f, 16).sliceCount != 1)
        return fail(name, "far <= near + offset is not a single slice");
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (makeForwardPlusGridZ(0.1f, inf, 16).sliceCount != 1 ||
        makeForwardPlusGridZ(0.1f, nan, 16).sliceCount != 1 ||
        makeForwardPlusGridZ(nan, 100.0f, 16).sliceCount != 1)
        return fail(name, "non-finite near/far is not a single slice");
    // Slice count is clamped to the 64-slice maximum.
    ForwardPlusGridZ clamped = makeForwardPlusGridZ(0.1f, 100.0f, 1000);
    if (clamped.sliceCount != kForwardPlusGridMaxSlices)
        return fail(name, "slice count is not clamped to 64");
    ForwardPlusGridZ grid = makeForwardPlusGridZ(0.1f, 100.0f, 16);
    if (grid.sliceCount != 16)
        return fail(name, "16-slice grid was not created");
    // Depths at/before the near offset, negative, and NaN depths are slice 0;
    // the far depth and beyond are the last slice.
    if (forwardPlusGridSlice(grid, 0.0f) != 0 || forwardPlusGridSlice(grid, -3.0f) != 0 ||
        forwardPlusGridSlice(grid, nan) != 0 || forwardPlusGridSlice(grid, 0.2f) != 0)
        return fail(name, "near/negative/NaN depth is not slice 0");
    if (forwardPlusGridSlice(grid, 100.0f) != 15 || forwardPlusGridSlice(grid, inf) != 15 ||
        forwardPlusGridSlice(grid, 1.0e6f) != 15)
        return fail(name, "far/beyond depth is not the last slice");
    ForwardPlusSliceRange single = forwardPlusLightSliceRange(oneSlice, 10.0f, 2.0f);
    if (single.first != 0 || single.last != 0)
        return fail(name, "single-slice light range is not {0,0}");
    return pass(name);
}

static bool testLightsExactlyOnSliceBoundaries()
{
    const std::string name = "depths exactly on slice boundaries start the higher slice";
    ForwardPlusGridZ grid = makeForwardPlusGridZ(0.1f, 100.0f, 16);
    if (grid.sliceCount != 16)
        return fail(name, "16-slice grid was not created");
    for (uint32_t k : {1u, 4u, 8u, 15u})
    {
        // Slice k starts where log2(depth * scale + offset) * distribution
        // reaches k, i.e. depth * scale + offset == 2^(k / distribution).
        const double boundary =
            (std::exp2(static_cast<double>(k) / grid.distribution) - grid.offset) / grid.scale;
        const float below = static_cast<float>(boundary * 0.999);
        const float above = static_cast<float>(boundary * 1.001);
        if (forwardPlusGridSlice(grid, below) != k - 1u)
            return fail(name, "depth just below a slice boundary is not in the lower slice");
        if (forwardPlusGridSlice(grid, above) != k)
            return fail(name, "depth just above a slice boundary is not in the higher slice");
    }
    // Monotonic over a dense sweep, and never out of range.
    uint32_t previous = 0;
    for (float depth = 0.05f; depth < 500.0f; depth *= 1.05f)
    {
        uint32_t slice = forwardPlusGridSlice(grid, depth);
        if (slice < previous || slice > 15)
            return fail(name, "slice sweep is not monotonic/bounded");
        previous = slice;
    }
    return pass(name);
}

static bool testSliceBoundaryLightSpansAdjacentSlices()
{
    const std::string name = "light centred on a slice boundary is binned into both adjacent slices";
    TestCamera cam;
    // Furthest sphere reaches depth 101, so the candidate grid is built for
    // far depth 101.1. Rebuild it exactly as the binning code does.
    ForwardPlusGridZ candidate = makeForwardPlusGridZ(0.1f, 101.1f, 16);
    if (candidate.sliceCount != 16)
        return fail(name, "candidate 16-slice grid was not created");
    const double boundary =
        (std::exp2(8.0 / candidate.distribution) - candidate.offset) / candidate.scale;
    std::vector<GPUPointLight> lights;
    for (int i = 0; i < 8; ++i) lights.push_back(makePoint(0.0f, 0.0f, -100.0f, 1.0f));
    for (int i = 0; i < 8; ++i) lights.push_back(makePoint(0.0f, 0.0f, -1.0f, 0.1f));
    lights.push_back(makePoint(0.0f, 0.0f, -static_cast<float>(boundary), 0.1f)); // index 16
    ForwardPlusBinningResult r = binForwardPlusLights(
        lights.data(), static_cast<uint32_t>(lights.size()), nullptr, 0, cam.view, cam.proj,
        cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (r.sliceCount != 16)
        return fail(name, "depth-spread scene was not z-sliced");
    ForwardPlusGridZ header = gridFromHeader(r);
    if (std::bit_cast<uint32_t>(header.scale) != std::bit_cast<uint32_t>(candidate.scale) ||
        std::bit_cast<uint32_t>(header.offset) != std::bit_cast<uint32_t>(candidate.offset) ||
        std::bit_cast<uint32_t>(header.distribution) != std::bit_cast<uint32_t>(candidate.distribution))
        return fail(name, "header grid does not match the candidate grid for far depth 101.1");
    // The slice range widens by radius + margin across the boundary, so the
    // probe touches slice 7 below and slice 8 above, and no other slice.
    if (slicesContaining(r, 16u) != std::set<uint32_t>{7u, 8u})
        return fail(name, "boundary-centred light is not in exactly slices {7,8}");
    return pass(name);
}

static bool testSpotIndexOffsetSpace()
{
    const std::string name = "spot indices are offset by point count, culled points included";
    TestCamera cam;
    // Spots alone start at index 0.
    GPUSpotLight spotsOnly[] = {makeSpot(0.0f, 0.0f, -5.0f, 0.5f), makeSpot(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult a = binForwardPlusLights(nullptr, 0, spotsOnly, 2, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    std::vector<uint32_t> centreA = indicesInCell(a, cellAt(a, 0, 5, 5));
    if (centreA != std::vector<uint32_t>{0u, 1u})
        return fail(name, "spots without points do not start at index 0");
    // Three behind-camera points are culled but still consume index space:
    // the visible spot is unified index 3 and the only index in the grid.
    GPUPointLight behind[] = {makePoint(0.0f, 0.0f, 5.0f, 0.5f), makePoint(0.0f, 0.0f, 6.0f, 0.5f),
                              makePoint(0.0f, 0.0f, 7.0f, 0.5f)};
    GPUSpotLight spot[] = {makeSpot(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult b = binForwardPlusLights(behind, 3, spot, 1, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (indicesInCell(b, cellAt(b, 0, 5, 5)) != std::vector<uint32_t>{3u} ||
        anyCellContains(b, 0u) || anyCellContains(b, 1u) || anyCellContains(b, 2u))
        return fail(name, "spot after culled points is not sole unified index 3");
    // An off-screen point likewise still offsets the spot.
    GPUPointLight offscreen[] = {makePoint(100.0f, 0.0f, -5.0f, 1.0f)};
    ForwardPlusBinningResult c = binForwardPlusLights(offscreen, 1, spot, 1, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (indicesInCell(c, cellAt(c, 0, 5, 5)) != std::vector<uint32_t>{1u})
        return fail(name, "spot after an off-screen point is not unified index 1");
    // Binning uses only the spot position/range sphere: direction and cone
    // angles do not change the result.
    GPUSpotLight facing[] = {makeSpot(0.0f, 0.0f, -5.0f, 0.5f, glm::vec3(0.0f, 0.0f, -1.0f))};
    GPUSpotLight backwards[] = {makeSpot(0.0f, 0.0f, -5.0f, 0.5f, glm::vec3(0.0f, 0.0f, 1.0f))};
    backwards[0].innerCutoff = 0.99f;
    backwards[0].outerCutoff = 0.5f;
    GPUPointLight onePoint[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult d = binForwardPlusLights(onePoint, 1, facing, 1, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    ForwardPlusBinningResult e = binForwardPlusLights(onePoint, 1, backwards, 1, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (!sameBinning(d, e) || indicesInCell(d, cellAt(d, 0, 5, 5)) != std::vector<uint32_t>{0u, 1u})
        return fail(name, "spot direction/cone changed its binning or index");
    return pass(name);
}

static bool testDisabledBinningMinimalGrid()
{
    const std::string name = "disabled binning returns the minimal grid for any viewport";
    TestCamera cam;
    GPUPointLight lights[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    BinningStats stats;
    ForwardPlusBinningResult r = binForwardPlusLights(lights, 1, nullptr, 0, cam.view, cam.proj,
                                                      161, 17, 0.1f, 1000.0f, true, false, &stats);
    if (!isMinimal(r, 11, 2))
        return fail(name, "disabled 161x17 result is not an 11x2 minimal grid");
    if (!statsAreZero(stats))
        return fail(name, "disabled binning does not leave stats zeroed");
    ForwardPlusBinningResult empty = binForwardPlusLights(lights, 1, nullptr, 0, cam.view, cam.proj,
                                                          0, 0, 0.1f, 1000.0f, true, false);
    if (!isMinimal(empty, 1, 1))
        return fail(name, "disabled 0x0 result is not a 1x1 minimal grid");
    // Even a slice-eligible light set stays single-slice while disabled.
    std::vector<GPUPointLight> spread;
    for (int i = 0; i < 8; ++i) spread.push_back(makePoint(0.0f, 0.0f, -1.0f, 0.1f));
    for (int i = 0; i < 8; ++i) spread.push_back(makePoint(0.0f, 0.0f, -100.0f, 1.0f));
    ForwardPlusBinningResult disabledSpread = binForwardPlusLights(
        spread.data(), static_cast<uint32_t>(spread.size()), nullptr, 0, cam.view, cam.proj,
        cam.width, cam.height, 0.1f, 1000.0f, true, false);
    if (!isMinimal(disabledSpread, 10, 10))
        return fail(name, "disabled depth-spread scene is not minimal");
    return pass(name);
}

static bool testZeroLights()
{
    const std::string name = "zero lights return the minimal grid at exact tile counts";
    TestCamera cam;
    BinningStats stats;
    ForwardPlusBinningResult nullArrays = binForwardPlusLights(nullptr, 0, nullptr, 0, cam.view, cam.proj,
                                                               cam.width, cam.height, 0.1f, 1000.0f,
                                                               true, true, &stats);
    if (!isMinimal(nullArrays, 10, 10) || !statsAreZero(stats))
        return fail(name, "null zero-light call is not minimal with zeroed stats");
    GPUPointLight noPoints[1] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    GPUSpotLight noSpots[1] = {makeSpot(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult emptyArrays = binForwardPlusLights(noPoints, 0, noSpots, 0, cam.view, cam.proj,
                                                                cam.width, cam.height, 0.1f, 1000.0f,
                                                                true, true);
    if (!isMinimal(emptyArrays, 10, 10))
        return fail(name, "non-null zero-count call is not minimal");
    // Tile counts are ceil(size / 16), clamped to at least one per axis.
    const struct { uint32_t w, h, tx, ty; } cases[] = {
        {0, 0, 1, 1}, {1, 1, 1, 1}, {15, 16, 1, 1}, {16, 16, 1, 1},
        {17, 16, 2, 1}, {160, 161, 10, 11}, {161, 17, 11, 2},
    };
    for (const auto& c : cases)
    {
        ForwardPlusBinningResult r = binForwardPlusLights(nullptr, 0, nullptr, 0, cam.view, cam.proj,
                                                          c.w, c.h, 0.1f, 1000.0f, true, true);
        if (!isMinimal(r, c.tx, c.ty))
            return fail(name, "zero-light tile count is not ceil(size/16) clamped to 1");
    }
    return pass(name);
}

static bool allCellsContainIndex(const ForwardPlusBinningResult& r, uint32_t index)
{
    if (r.lightIndices.size() != static_cast<size_t>(r.tilesX) * r.tilesY * r.sliceCount)
        return false;
    for (size_t i = 1; i < r.tiles.size(); ++i)
        if (r.tiles[i].lightCount != 1 || !cellContains(r, r.tiles[i], index))
            return false;
    return true;
}

static bool testBehindCameraAndEnclosingCamera()
{
    const std::string name = "behind-camera boundary, enclosing and near-camera lights";
    TestCamera cam;
    auto binOne = [&](const GPUPointLight& light) {
        return binForwardPlusLights(&light, 1, nullptr, 0, cam.view, cam.proj,
                                    cam.width, cam.height, 0.1f, 1000.0f, true, true);
    };
    // Centre exactly radius behind the camera: clip w == -radius, fully
    // behind, excluded.
    if (!isMinimal(binOne(makePoint(0.0f, 0.0f, 0.5f, 0.5f)), 10, 10))
        return fail(name, "light exactly radius behind the camera was binned");
    if (!isMinimal(binOne(makePoint(0.0f, 0.0f, 5.0f, 0.5f)), 10, 10))
        return fail(name, "light fully behind the camera was binned");
    // A spot behind the camera is excluded the same way.
    GPUSpotLight behindSpot[] = {makeSpot(0.0f, 0.0f, 5.0f, 0.5f)};
    ForwardPlusBinningResult spotResult = binForwardPlusLights(nullptr, 0, behindSpot, 1, cam.view, cam.proj,
                                                               cam.width, cam.height, 0.1f, 1000.0f,
                                                               true, true);
    if (!isMinimal(spotResult, 10, 10))
        return fail(name, "spot fully behind the camera was binned");
    // One centimetre closer: the sphere crosses the camera plane and the
    // bounds become the full grid.
    if (!allCellsContainIndex(binOne(makePoint(0.0f, 0.0f, 0.49f, 0.5f)), 0u))
        return fail(name, "sphere crossing the camera plane is not full-screen");
    if (!allCellsContainIndex(binOne(makePoint(0.0f, 0.0f, -0.5f, 2.0f)), 0u))
        return fail(name, "camera-enclosing light is not full-screen");
    if (!allCellsContainIndex(binOne(makePoint(0.0f, 0.0f, 0.0f, 0.5f)), 0u))
        return fail(name, "light centred on the camera is not full-screen");
    // The near-camera shortcut uses max(radius, 1): a small light 0.8 in
    // front, whose sphere does not reach the camera, is also full-screen.
    if (!allCellsContainIndex(binOne(makePoint(0.0f, 0.0f, -0.8f, 0.1f)), 0u))
        return fail(name, "light within one unit in front is not conservatively full-screen");
    // An ordinary in-front light does not touch the corner tiles.
    ForwardPlusBinningResult normal = binOne(makePoint(0.0f, 0.0f, -5.0f, 0.5f));
    if (cellAt(normal, 0, 0, 0).lightCount != 0 || cellAt(normal, 0, 9, 9).lightCount != 0 ||
        !cellContains(normal, cellAt(normal, 0, 5, 5), 0u))
        return fail(name, "ordinary in-front light coverage is wrong");
    return pass(name);
}

static bool testDegenerateLightsExcluded()
{
    const std::string name = "degenerate lights and null arrays are excluded without crashing";
    TestCamera cam;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    GPUPointLight points[] = {makePoint(0.0f, 0.0f, -5.0f, 0.0f), makePoint(0.0f, 0.0f, -5.0f, -1.0f),
                              makePoint(0.0f, 0.0f, -5.0f, nan), makePoint(nan, 0.0f, -5.0f, 1.0f)};
    GPUSpotLight spots[] = {makeSpot(0.0f, 0.0f, -5.0f, 0.0f), makeSpot(0.0f, 0.0f, -5.0f, -2.0f)};
    for (uint32_t i = 0; i < 4; ++i)
    {
        ForwardPlusBinningResult single = binForwardPlusLights(points + i, 1, nullptr, 0, cam.view, cam.proj,
                                                               cam.width, cam.height, 0.1f, 1000.0f,
                                                               true, true);
        if (!isMinimal(single, 10, 10))
            return fail(name, "degenerate point light " + std::to_string(i) + " was binned");
    }
    for (uint32_t i = 0; i < 2; ++i)
    {
        ForwardPlusBinningResult single = binForwardPlusLights(nullptr, 0, spots + i, 1, cam.view, cam.proj,
                                                               cam.width, cam.height, 0.1f, 1000.0f,
                                                               true, true);
        if (!isMinimal(single, 10, 10))
            return fail(name, "degenerate spot light " + std::to_string(i) + " was binned");
    }
    ForwardPlusBinningResult r = binForwardPlusLights(points, 4, spots, 2, cam.view, cam.proj,
                                                      cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (!isMinimal(r, 10, 10))
        return fail(name, "zero/negative/NaN-range or NaN-position lights were binned together");
    // Null arrays with non-zero counts are skipped by the guarded loops.
    ForwardPlusBinningResult nulls = binForwardPlusLights(nullptr, 3, nullptr, 2, cam.view, cam.proj,
                                                          cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (!isMinimal(nulls, 10, 10))
        return fail(name, "null light arrays with counts were not skipped");
    // A zero-sized viewport bins nothing but still reports a 1-wide grid.
    GPUPointLight light[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult noWidth = binForwardPlusLights(light, 1, nullptr, 0, cam.view, cam.proj,
                                                            0, 160, 0.1f, 1000.0f, true, true);
    ForwardPlusBinningResult noHeight = binForwardPlusLights(light, 1, nullptr, 0, cam.view, cam.proj,
                                                             160, 0, 0.1f, 1000.0f, true, true);
    if (!isMinimal(noWidth, 1, 10) || !isMinimal(noHeight, 10, 1))
        return fail(name, "zero width/height does not return the clamped minimal grid");
    // Direct bounds reject the same degenerates.
    if (lightTileBounds(glm::vec3(0.0f, 0.0f, -5.0f), 0.0f, cam.proj, true, 160, 160, 10, 10, 16, true) ||
        lightTileBounds(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f, cam.proj, true, 0, 160, 10, 10, 16, true) ||
        lightTileBounds(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f, cam.proj, true, 160, 160, 0, 10, 16, true))
        return fail(name, "lightTileBounds accepts radius 0 or a zero dimension");
    return pass(name);
}

// Recompute the expected grid from the public bounds/slice helpers and the
// header grid, then compare cell contents and stats exactly. This checks
// counting, prefix sums, scatter order and truncation on any scene.
static bool verifyGridInvariants(const std::string& name, const ForwardPlusBinningResult& r,
                                 const BinningStats& stats, const GPUPointLight* points, uint32_t pointCount,
                                 const GPUSpotLight* spots, uint32_t spotCount, const TestCamera& cam,
                                 uint32_t width, uint32_t height, bool flipY, bool enabled)
{
    const uint32_t total = pointCount + spotCount;
    if (!enabled || total == 0)
    {
        if (!isMinimal(r, r.tilesX, r.tilesY) || !statsAreZero(stats))
            return fail(name, "disabled/empty scene is not minimal with zeroed stats");
        return true;
    }
    const glm::mat4 viewProj = cam.proj * cam.view;
    const bool perspective = cam.proj[2][3] != 0.0f;
    ForwardPlusGridZ grid = gridFromHeader(r);
    if (grid.sliceCount != r.sliceCount)
        return fail(name, "header slice count disagrees with the result");
    const uint32_t tileCount = r.tilesX * r.tilesY;
    if (r.tiles.size() != static_cast<size_t>(tileCount) * r.sliceCount + 1u)
        return fail(name, "tile buffer size is not 1 + tiles * slices");
    std::vector<std::vector<uint32_t>> expected(static_cast<size_t>(tileCount) * r.sliceCount);
    uint64_t binnedCount = 0;
    auto addLight = [&](uint32_t index, const glm::vec3& position, float range) {
        std::optional<ForwardPlusTileBounds> bounds =
            lightTileBounds(position, range, viewProj, perspective, width, height, r.tilesX, r.tilesY,
                            kForwardPlusTileSize, flipY);
        if (!bounds) return;
        ++binnedCount;
        const glm::vec4 viewPos = cam.view * glm::vec4(position, 1.0f);
        ForwardPlusSliceRange slices = forwardPlusLightSliceRange(grid, -viewPos.z, range);
        for (uint32_t slice = slices.first; slice <= slices.last; ++slice)
            for (uint32_t ty = bounds->minY; ty <= bounds->maxY; ++ty)
                for (uint32_t tx = bounds->minX; tx <= bounds->maxX; ++tx)
                    expected[(slice * tileCount) + (ty * r.tilesX) + tx].push_back(index);
    };
    if (points)
        for (uint32_t i = 0; i < pointCount; ++i) addLight(i, points[i].position, points[i].range);
    if (spots)
        for (uint32_t i = 0; i < spotCount; ++i) addLight(pointCount + i, spots[i].position, spots[i].range);
    if (binnedCount == 0)
    {
        if (!isMinimal(r, r.tilesX, r.tilesY) || !statsAreZero(stats))
            return fail(name, "all-culled scene is not minimal with zeroed stats");
        return true;
    }
    uint32_t running = 0;
    uint64_t truncated = 0;
    uint64_t touched = 0;
    uint32_t maxOccupancy = 0;
    for (size_t cell = 0; cell < expected.size(); ++cell)
    {
        const ForwardPlusTile& tile = r.tiles[1u + cell];
        const uint32_t capped =
            (std::min)(static_cast<uint32_t>(expected[cell].size()), kForwardPlusMaxLightsPerTile);
        truncated += expected[cell].size() - capped;
        if (capped > 0) ++touched;
        maxOccupancy = (std::max)(maxOccupancy, capped);
        if (tile.lightOffset != running || tile.lightCount != capped)
            return fail(name, "cell offset/count is not the capped prefix sum");
        running += capped;
        std::vector<uint32_t> actual = indicesInCell(r, tile);
        std::vector<uint32_t> want(expected[cell].begin(), expected[cell].begin() + capped);
        if (actual != want)
            return fail(name, "cell contents are not the lowest unified indices in order");
    }
    if (r.lightIndices.size() != running)
        return fail(name, "index buffer size is not the sum of capped cell counts");
    if (stats.lights_in != total || stats.lights_binned != binnedCount ||
        stats.indices_written != r.lightIndices.size() || stats.lights_truncated != truncated ||
        stats.tiles_touched != touched || stats.max_tile_occupancy != maxOccupancy)
        return fail(name, "stats do not match the recomputed grid");
    return true;
}

static bool testRetainedScratchRandomizedMatchesOneOff()
{
    const std::string name = "retained scratch matches one-off output on randomized scenes (fixed seed)";
    TestCamera cam;
    std::mt19937 rng(0xB1B1u);
    std::uniform_real_distribution<float> posXY(-8.0f, 8.0f);
    std::uniform_real_distribution<float> posZ(-30.0f, 3.0f);
    std::uniform_real_distribution<float> rangeDist(0.0f, 5.0f);
    const uint32_t widths[] = {160, 160, 64, 161, 17};
    const uint32_t heights[] = {160, 96, 160, 17, 160};
    ForwardPlusBinningResult reused;
    ForwardPlusBinningScratch scratch;
    for (uint32_t scene = 0; scene < 150; ++scene)
    {
        const std::string sceneName = name + " scene " + std::to_string(scene);
        const uint32_t pointCount = rng() % 33;
        const uint32_t spotCount = rng() % 17;
        std::vector<GPUPointLight> points;
        std::vector<GPUSpotLight> spots;
        for (uint32_t i = 0; i < pointCount; ++i)
        {
            float range = (i % 17 == 0) ? 0.0f : rangeDist(rng);
            points.push_back(makePoint(posXY(rng), posXY(rng), posZ(rng), range));
        }
        for (uint32_t i = 0; i < spotCount; ++i)
            spots.push_back(makeSpot(posXY(rng), posXY(rng), posZ(rng), rangeDist(rng)));
        const uint32_t width = widths[scene % 5];
        const uint32_t height = heights[scene % 5];
        const bool flipY = (rng() % 2) == 0;
        const bool enabled = (rng() % 8) != 0;
        const GPUPointLight* pointPtr = points.empty() ? nullptr : points.data();
        const GPUSpotLight* spotPtr = spots.empty() ? nullptr : spots.data();
        BinningStats freshStats;
        BinningStats reusedStats;
        ForwardPlusBinningResult fresh = binForwardPlusLights(
            pointPtr, pointCount, spotPtr, spotCount, cam.view, cam.proj, width, height,
            0.1f, 1000.0f, flipY, enabled, &freshStats);
        binForwardPlusLightsInto(reused, scratch, pointPtr, pointCount, spotPtr, spotCount,
                                 cam.view, cam.proj, width, height, 0.1f, 1000.0f, flipY, enabled,
                                 &reusedStats);
        if (!sameBinning(reused, fresh) || !statsEqual(reusedStats, freshStats))
            return fail(sceneName, "retained-scratch result differs from the one-off result");
        if (!verifyGridInvariants(sceneName, reused, reusedStats, pointPtr, pointCount, spotPtr, spotCount,
                                  cam, width, height, flipY, enabled))
            return false;
    }
    // State change after the randomized run: a zero-light call and a disabled
    // call through the same retained storage must both clear to minimal.
    ForwardPlusBinningResult freshEmpty = binForwardPlusLights(nullptr, 0, nullptr, 0, cam.view, cam.proj,
                                                               cam.width, cam.height, 0.1f, 1000.0f,
                                                               true, true);
    binForwardPlusLightsInto(reused, scratch, nullptr, 0, nullptr, 0, cam.view, cam.proj,
                             cam.width, cam.height, 0.1f, 1000.0f, true, true);
    if (!sameBinning(reused, freshEmpty) || !isMinimal(reused, 10, 10))
        return fail(name, "zero-light call after randomized scenes leaves stale output");
    GPUPointLight one[] = {makePoint(0.0f, 0.0f, -5.0f, 0.5f)};
    ForwardPlusBinningResult freshDisabled = binForwardPlusLights(one, 1, nullptr, 0, cam.view, cam.proj,
                                                                  cam.width, cam.height, 0.1f, 1000.0f,
                                                                  true, false);
    binForwardPlusLightsInto(reused, scratch, one, 1, nullptr, 0, cam.view, cam.proj,
                             cam.width, cam.height, 0.1f, 1000.0f, true, false);
    if (!sameBinning(reused, freshDisabled) || !isMinimal(reused, 10, 10))
        return fail(name, "disabled call after randomized scenes leaves stale output");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testPerTileCapExactTruncation();
    ok &= testCapBoundary64And65AndUnifiedSpotCrossing();
    ok &= testLightsExactlyOnTileBoundaries();
    ok &= testScreenEdgeInclusion();
    ok &= testGridZDegenerateParameters();
    ok &= testLightsExactlyOnSliceBoundaries();
    ok &= testSliceBoundaryLightSpansAdjacentSlices();
    ok &= testSpotIndexOffsetSpace();
    ok &= testDisabledBinningMinimalGrid();
    ok &= testZeroLights();
    ok &= testBehindCameraAndEnclosingCamera();
    ok &= testDegenerateLightsExcluded();
    ok &= testRetainedScratchRandomizedMatchesOneOff();
    std::cout << (ok ? "all BinningEdge tests passed" : "BinningEdge tests FAILED") << std::endl;
    return ok ? 0 : 1;
}



