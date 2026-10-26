#pragma once

// Forward+ light binning (CPU tile / z-slice binning), shared by the Vulkan,
// D3D12 and Metal backends. Ported from OpenStrike/Ferrite
// (FerriteEngine/private/FerriteClient/renderer/rhi/rhi_render_constants.hpp
// light_tile_bounds / forward_plus_grid_z, and
// FerriteEngine/src/renderer/rhi/passes/rhi_world_pass_forward_plus.cpp):
//  * each point/spot light's cull sphere is projected to a screen-space tile
//    rectangle (7-point sphere samples, full-screen when the sphere encloses
//    the camera, culled when fully behind/off-screen);
//  * the view depth range is optionally cut into exponential z slices
//    (slice = floor(log2(depth * scale + offset) * distribution)), but only
//    when slicing can pay: perspective camera, enough binned lights, and a
//    light set whose spheres span a small share of the view depth;
//  * light indices are counted per cell (capped), prefix-summed into cell
//    offsets, then scattered. Element 0 of the tile buffer is a grid header
//    with the same layout as a tile; cells follow it, slice-major.
// The shader side of the grid lookup lives in assets/shaders/slang/common.slang
// (ForwardPlusTile, forwardPlusGridSlice, forwardPlusCellAt) and must round
// the same way as the functions below.
//
// Screen-space convention: tile (0,0) is the tile containing the fragment at
// input.position.xy == (0,0) in each shader. `flipY` selects how projected
// NDC y becomes screen y (see lightTileBounds); each backend passes the flip
// matching its own viewport / fragcoord setup.

#include "Graphics/RenderAPI.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// One cell of the Forward+ light grid: the span {lightOffset, lightCount} of
// the light index buffer (16 bytes, mirrors ForwardPlusTile in common.slang).
// Element 0 of the tile buffer is a grid header with the same layout:
// lightOffset = sliceCount, lightCount = bitcast(zScale),
// padding0 = bitcast(zOffset), padding1 = bitcast(zDistribution).
struct ForwardPlusTile
{
    uint32_t lightOffset = 0;
    uint32_t lightCount = 0;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
};
static_assert(sizeof(ForwardPlusTile) == 16, "ForwardPlusTile must be 16 bytes");

inline constexpr uint32_t kForwardPlusTileSize = 16;
inline constexpr uint32_t kForwardPlusMaxLightsPerTile = 64;
inline constexpr uint32_t kForwardPlusGridSizeZ = 16;
inline constexpr uint32_t kForwardPlusGridMinLights = 8;
inline constexpr float kForwardPlusGridMaxCoverage = 0.5f;
// Upper bound of tiles x slices (16 bytes a cell): a larger tile grid gets
// fewer slices, never a larger buffer.
inline constexpr uint32_t kForwardPlusGridMaxCells = 1u << 19;
inline constexpr float kForwardPlusGridDepthDistribution = 4.0f; // slices per doubling of depth
inline constexpr float kForwardPlusGridNearOffset = 0.1f;        // Garden near=0.1 far=1000, metre units
inline constexpr uint32_t kForwardPlusGridMaxSlices = 64;

// Exponential z-slice parameters. The slice of a view depth z is
// floor(log2(z * scale + offset) * distribution) clamped to sliceCount - 1;
// the parameters put slice 0 at the near plane (plus a small offset) and the
// start of the LAST slice at the far depth.
struct ForwardPlusGridZ
{
    uint32_t sliceCount = 1;
    float scale = 0.0f;
    float offset = 1.0f;
    float distribution = 0.0f;
};

// The grid's Z parameters for `sliceCount` slices from `nearPlane` to
// `farPlane` (view depth). Fewer than two slices, or a far depth that leaves
// no range after the near offset, is the single-slice (2D tile) grid.
inline ForwardPlusGridZ makeForwardPlusGridZ(float nearPlane, float farPlane, uint32_t sliceCount)
{
    ForwardPlusGridZ params;
    sliceCount = (std::min)(sliceCount, kForwardPlusGridMaxSlices);
    const double n = static_cast<double>(nearPlane) + static_cast<double>(kForwardPlusGridNearOffset);
    const double f = static_cast<double>(farPlane);
    if (sliceCount < 2u || !(f > n) || !std::isfinite(f) || !std::isfinite(n)) return params;
    const double s = static_cast<double>(kForwardPlusGridDepthDistribution);
    // The last slice starts at the far depth (sliceCount - 1 steps of the exponential).
    const double o = (f - n * std::exp2(static_cast<double>(sliceCount - 1u) / s)) / (f - n);
    const double b = (1.0 - o) / n;
    params.sliceCount = sliceCount;
    params.scale = static_cast<float>(b);
    params.offset = static_cast<float>(o);
    params.distribution = static_cast<float>(s);
    return params;
}

// The slice of a view depth (positive in front of the camera, i.e. -viewZ).
// A depth before the near plane, or a non-finite one, is slice 0.
inline uint32_t forwardPlusGridSlice(const ForwardPlusGridZ& params, float viewDepth)
{
    if (params.sliceCount <= 1u) return 0u;
    const float term = viewDepth * params.scale + params.offset;
    const float slice = std::log2(term > 1.0f ? term : 1.0f) * params.distribution;
    const float last = static_cast<float>(params.sliceCount - 1u);
    return slice >= last ? params.sliceCount - 1u : (slice > 0.0f ? static_cast<uint32_t>(slice) : 0u);
}

struct ForwardPlusSliceRange
{
    uint32_t first = 0;
    uint32_t last = 0;
};

// The slices a sphere (view depth of its centre, radius) can touch: a pixel
// at distance <= radius from the centre has a view depth within radius of the
// centre depth. The range is widened by a relative margin so a pixel whose
// depth the GPU rounds across a slice boundary differently from the CPU never
// loses a light it should receive.
inline ForwardPlusSliceRange forwardPlusLightSliceRange(const ForwardPlusGridZ& params, float centerDepth,
                                                        float radius)
{
    if (params.sliceCount <= 1u) return {};
    const float margin = 1.0e-3f * (std::fabs(centerDepth) + radius) + 0.01f;
    return {forwardPlusGridSlice(params, centerDepth - radius - margin),
            forwardPlusGridSlice(params, centerDepth + radius + margin)};
}

struct ForwardPlusTileBounds
{
    uint32_t minX = 0;
    uint32_t maxX = 0;
    uint32_t minY = 0;
    uint32_t maxY = 0;
};

// Screen-space tile rectangle covered by a light's cull sphere, in SHADER
// FRAGCOORD space: tile (0,0) contains the fragment whose input.position.xy
// is (0,0). `flipY` converts projected NDC y to screen y:
//   flipY == true:  screenY = (0.5f - ndcY * 0.5f) * height
//   flipY == false: screenY = (ndcY * 0.5f + 0.5f) * height
// Each backend passes the flip matching its viewport/fragcoord setup (e.g. a
// projection that already flips Y, such as Vulkan's negative viewport-height
// or [1][1] *= -1 projection, pairs with flipY == false; a conventional
// y-up projection rendered into a top-left-origin framebuffer, such as
// D3D12/Metal, pairs with flipY == true).
// Returns nullopt when the sphere is fully behind the camera or off-screen;
// a sphere whose centre is closer than its radius returns the full grid.
inline std::optional<ForwardPlusTileBounds> lightTileBounds(const glm::vec3& position, float radius,
                                                            const glm::mat4& viewProj, bool perspective,
                                                            uint32_t width, uint32_t height, uint32_t tilesX,
                                                            uint32_t tilesY, uint32_t tileSize, bool flipY)
{
    if (width == 0 || height == 0 || tilesX == 0 || tilesY == 0 || !(radius > 0.0f))
        return std::nullopt;

    auto fullScreen = [&]() {
        return ForwardPlusTileBounds{0, tilesX - 1u, 0, tilesY - 1u};
    };
    auto projectToScreen = [&](const glm::vec3& point, float& screenX, float& screenY) {
        const glm::vec4 clip = viewProj * glm::vec4(point, 1.0f);
        if (clip.w <= 0.001f) return false;
        const float ndcX = clip.x / clip.w;
        const float ndcY = clip.y / clip.w;
        screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
        screenY = flipY ? (0.5f - ndcY * 0.5f) * static_cast<float>(height)
                        : (ndcY * 0.5f + 0.5f) * static_cast<float>(height);
        return std::isfinite(screenX) && std::isfinite(screenY);
    };

    const glm::vec4 centerClip = viewProj * glm::vec4(position, 1.0f);
    if (perspective && centerClip.w <= -radius)
        return std::nullopt;
    if (perspective && centerClip.w <= (std::max)(radius, 1.0f))
        return fullScreen();

    const glm::vec3 samples[7] = {
        position,
        {position.x - radius, position.y, position.z},
        {position.x + radius, position.y, position.z},
        {position.x, position.y - radius, position.z},
        {position.x, position.y + radius, position.z},
        {position.x, position.y, position.z - radius},
        {position.x, position.y, position.z + radius},
    };

    float minX = (std::numeric_limits<float>::max)();
    float minY = (std::numeric_limits<float>::max)();
    float maxX = -(std::numeric_limits<float>::max)();
    float maxY = -(std::numeric_limits<float>::max)();
    bool anyProjected = false;
    for (const glm::vec3& sample : samples)
    {
        float screenX = 0.0f;
        float screenY = 0.0f;
        if (!projectToScreen(sample, screenX, screenY))
            continue;
        anyProjected = true;
        minX = (std::min)(minX, screenX);
        minY = (std::min)(minY, screenY);
        maxX = (std::max)(maxX, screenX);
        maxY = (std::max)(maxY, screenY);
    }

    if (!anyProjected || maxX < 0.0f || maxY < 0.0f || minX >= static_cast<float>(width) ||
        minY >= static_cast<float>(height))
        return std::nullopt;

    minX = std::clamp(minX, 0.0f, static_cast<float>(width - 1u));
    maxX = std::clamp(maxX, 0.0f, static_cast<float>(width - 1u));
    minY = std::clamp(minY, 0.0f, static_cast<float>(height - 1u));
    maxY = std::clamp(maxY, 0.0f, static_cast<float>(height - 1u));

    const float tileSizeF = static_cast<float>((std::max)(tileSize, 1u));
    return ForwardPlusTileBounds{
        static_cast<uint32_t>(std::floor(minX / tileSizeF)),
        (std::min)(tilesX - 1u, static_cast<uint32_t>(std::floor(maxX / tileSizeF))),
        static_cast<uint32_t>(std::floor(minY / tileSizeF)),
        (std::min)(tilesY - 1u, static_cast<uint32_t>(std::floor(maxY / tileSizeF))),
    };
}

struct BinningStats
{
    uint64_t lights_in = 0;
    uint64_t lights_binned = 0;
    uint64_t tiles_touched = 0;
    uint64_t indices_written = 0;
    uint64_t lights_truncated = 0;
    uint64_t max_tile_occupancy = 0;
};

struct ForwardPlusBinningResult
{
    std::vector<ForwardPlusTile> tiles;       // element 0 is the grid header; cells follow, slice-major
    std::vector<uint32_t> lightIndices;       // unified light indices (see binForwardPlusLights)
    uint32_t tilesX = 1;
    uint32_t tilesY = 1;
    uint32_t sliceCount = 1;
};

// One visible light prepared for binning, retained between calls so a
// caller that bins every frame reuses the same storage.
struct ForwardPlusBinnedLight
{
    uint32_t index = 0;
    ForwardPlusTileBounds bounds{};
    float viewDepth = 0.0f;
    float radius = 0.0f;
    ForwardPlusSliceRange slices{};
};

// Working storage for binForwardPlusLightsInto. Clearing it between calls
// keeps the allocated capacity, so repeated binning of a similar light set
// does not allocate again.
struct ForwardPlusBinningScratch
{
    std::vector<ForwardPlusBinnedLight> binned;
    std::vector<uint32_t> cellCounts;
    std::vector<uint32_t> writeOffsets;

    void clear()
    {
        binned.clear();
        cellCounts.clear();
        writeOffsets.clear();
    }
};

// Bins point + spot lights into the Forward+ tile/z-slice grid.
// Unified light index space: point lights are indices [0, pointCount), spot
// lights are [pointCount, pointCount + spotCount); each light's cull sphere
// is (position, range). `viewMatrix`/`projMatrix` are the camera matrices the
// frame is drawn with (view depth = -viewZ, positive in front); `flipY` is
// forwarded to lightTileBounds. When `enabled` is false, or there are no
// lights, a valid minimal result is still returned (header with sliceCount=1
// and a zeroed tile grid whose first cell is offset 0 / count 0, tilesX/tilesY
// computed, empty lightIndices) so backends can always bind something.
inline void binForwardPlusLightsInto(ForwardPlusBinningResult& result, ForwardPlusBinningScratch& scratch,
                                     const GPUPointLight* points, uint32_t pointCount,
                                     const GPUSpotLight* spots, uint32_t spotCount,
                                     const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                     uint32_t width, uint32_t height, float nearPlane,
                                     float farPlane, bool flipY, bool enabled,
                                     BinningStats* stats = nullptr)
{
    if (stats) *stats = BinningStats{};
    scratch.clear();
    result.tilesX = (std::max)(1u, (width + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);
    result.tilesY = (std::max)(1u, (height + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);
    const uint32_t tileCount = result.tilesX * result.tilesY;

    auto finishMinimal = [&]() {
        result.sliceCount = 1;
        result.tiles.assign(static_cast<size_t>(tileCount) + 1u, ForwardPlusTile{});
        ForwardPlusGridZ grid; // single slice: scale 0, offset 1, distribution 0
        result.tiles[0].lightOffset = grid.sliceCount;
        result.tiles[0].lightCount = std::bit_cast<uint32_t>(grid.scale);
        result.tiles[0].padding0 = std::bit_cast<uint32_t>(grid.offset);
        result.tiles[0].padding1 = std::bit_cast<uint32_t>(grid.distribution);
        result.lightIndices.clear();
    };

    const uint32_t totalLights = pointCount + spotCount;
    if (!enabled || totalLights == 0)
    {
        finishMinimal();
        return;
    }

    const glm::mat4 viewProj = projMatrix * viewMatrix;
    // glm perspective projections put -1 in column 2, row 3; orthographic puts 0.
    const bool perspective = projMatrix[2][3] != 0.0f;

    std::vector<ForwardPlusBinnedLight>& binned = scratch.binned;
    binned.reserve(totalLights);
    float furthestDepth = 0.0f;

    auto consider = [&](uint32_t index, const glm::vec3& position, float radius) {
        const std::optional<ForwardPlusTileBounds> bounds =
            lightTileBounds(position, radius, viewProj, perspective, width, height, result.tilesX,
                            result.tilesY, kForwardPlusTileSize, flipY);
        if (!bounds) return;
        const glm::vec4 viewPos = viewMatrix * glm::vec4(position, 1.0f);
        ForwardPlusBinnedLight light;
        light.index = index;
        light.bounds = *bounds;
        light.viewDepth = -viewPos.z;
        light.radius = radius;
        binned.push_back(light);
        furthestDepth = (std::max)(furthestDepth, light.viewDepth + radius);
    };
    if (points)
        for (uint32_t i = 0; i < pointCount; ++i) consider(i, points[i].position, points[i].range);
    if (spots)
        for (uint32_t i = 0; i < spotCount; ++i)
            consider(pointCount + i, spots[i].position, spots[i].range);

    if (binned.empty())
    {
        finishMinimal();
        return;
    }

    // Z slicing only when it can pay: a perspective camera, enough binned
    // lights, and a light set whose spheres span a small share of the view
    // depth (area-weighted slice coverage <= kForwardPlusGridMaxCoverage).
    ForwardPlusGridZ grid;
    const uint32_t maxSlicesByCells = (std::max)(1u, kForwardPlusGridMaxCells / (std::max)(tileCount, 1u));
    const uint32_t requestedSlices =
        (std::min)({kForwardPlusGridSizeZ, maxSlicesByCells, kForwardPlusGridMaxSlices});
    if (perspective && requestedSlices > 1u && binned.size() >= kForwardPlusGridMinLights)
    {
        const float farDepth = (std::min)(furthestDepth + kForwardPlusGridNearOffset, farPlane);
        const ForwardPlusGridZ candidate = makeForwardPlusGridZ(nearPlane, farDepth, requestedSlices);
        if (candidate.sliceCount > 1u)
        {
            double weightedSpan = 0.0;
            double weightedArea = 0.0;
            for (const ForwardPlusBinnedLight& light : binned)
            {
                const ForwardPlusSliceRange range =
                    forwardPlusLightSliceRange(candidate, light.viewDepth, light.radius);
                const double area = static_cast<double>(light.bounds.maxX - light.bounds.minX + 1u) *
                                    static_cast<double>(light.bounds.maxY - light.bounds.minY + 1u);
                weightedSpan += area * static_cast<double>(range.last - range.first + 1u);
                weightedArea += area;
            }
            const float coverage = weightedArea > 0.0
                                       ? static_cast<float>(weightedSpan /
                                                              (weightedArea * candidate.sliceCount))
                                       : 1.0f;
            if (coverage <= kForwardPlusGridMaxCoverage)
                grid = candidate;
        }
    }
    for (ForwardPlusBinnedLight& light : binned)
        light.slices = forwardPlusLightSliceRange(grid, light.viewDepth, light.radius);

    result.sliceCount = grid.sliceCount;
    const uint32_t cellCount = tileCount * grid.sliceCount;

    // Count lights per cell, capped at kForwardPlusMaxLightsPerTile (the cap
    // keeps the lowest slots, as the scatter below does).
    std::vector<uint32_t>& cellCounts = scratch.cellCounts;
    cellCounts.assign(cellCount, 0u);
    for (const ForwardPlusBinnedLight& light : binned)
        for (uint32_t slice = light.slices.first; slice <= light.slices.last; ++slice)
            for (uint32_t ty = light.bounds.minY; ty <= light.bounds.maxY; ++ty)
                for (uint32_t tx = light.bounds.minX; tx <= light.bounds.maxX; ++tx)
                    ++cellCounts[(slice * tileCount) + (ty * result.tilesX) + tx];
    uint64_t truncatedCount = 0;
    for (uint32_t c : cellCounts)
        if (c > kForwardPlusMaxLightsPerTile) truncatedCount += (uint64_t)(c - kForwardPlusMaxLightsPerTile);
    for (uint32_t& count : cellCounts)
        count = (std::min)(count, kForwardPlusMaxLightsPerTile);

    // Prefix-sum the capped counts into cell offsets; element 0 is the header.
    result.tiles.assign(static_cast<size_t>(cellCount) + 1u, ForwardPlusTile{});
    result.tiles[0].lightOffset = grid.sliceCount;
    result.tiles[0].lightCount = std::bit_cast<uint32_t>(grid.scale);
    result.tiles[0].padding0 = std::bit_cast<uint32_t>(grid.offset);
    result.tiles[0].padding1 = std::bit_cast<uint32_t>(grid.distribution);
    uint32_t runningOffset = 0;
    std::vector<uint32_t>& writeOffsets = scratch.writeOffsets;
    writeOffsets.assign(cellCount, 0u);
    for (uint32_t cell = 0; cell < cellCount; ++cell)
    {
        ForwardPlusTile& tile = result.tiles[1u + cell];
        tile.lightOffset = runningOffset;
        tile.lightCount = cellCounts[cell];
        writeOffsets[cell] = runningOffset;
        runningOffset += cellCounts[cell];
    }

    // Scatter light indices, capped per cell.
    result.lightIndices.assign(runningOffset, 0u);
    for (const ForwardPlusBinnedLight& light : binned)
    {
        for (uint32_t slice = light.slices.first; slice <= light.slices.last; ++slice)
        {
            for (uint32_t ty = light.bounds.minY; ty <= light.bounds.maxY; ++ty)
            {
                for (uint32_t tx = light.bounds.minX; tx <= light.bounds.maxX; ++tx)
                {
                    const uint32_t cell = (slice * tileCount) + (ty * result.tilesX) + tx;
                    const ForwardPlusTile& tile = result.tiles[1u + cell];
                    uint32_t& write = writeOffsets[cell];
                    if (write < tile.lightOffset + tile.lightCount)
                        result.lightIndices[write++] = light.index;
                }
            }
        }
    }
    if (stats)
    {
        stats->lights_in = (uint64_t)pointCount + (uint64_t)spotCount;
        stats->lights_binned = (uint64_t)binned.size();
        stats->indices_written = (uint64_t)result.lightIndices.size();
        uint64_t touched = 0;
        uint64_t maxOcc = 0;
        for (size_t ci = 0; ci < cellCounts.size(); ++ci)
        {
            if (cellCounts[ci] > 0) ++touched;
            if ((uint64_t)cellCounts[ci] > maxOcc) maxOcc = (uint64_t)cellCounts[ci];
        }
        stats->tiles_touched = touched;
        stats->max_tile_occupancy = maxOcc;
        stats->lights_truncated = truncatedCount;
        uint64_t uncapped = 0;
        // recompute uncapped total from capped cells is not available here; truncated is derived below
        (void)uncapped;
    }
}

// Binning entry point for one-off calls. Callers that bin every frame keep
// a result and scratch and call binForwardPlusLightsInto instead, so the
// working storage is reused instead of allocated per call.
inline ForwardPlusBinningResult binForwardPlusLights(const GPUPointLight* points, uint32_t pointCount,
                                                     const GPUSpotLight* spots, uint32_t spotCount,
                                                     const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                                     uint32_t width, uint32_t height, float nearPlane,
                                                     float farPlane, bool flipY, bool enabled,
                                                     BinningStats* stats = nullptr)
{
    ForwardPlusBinningResult result;
    ForwardPlusBinningScratch scratch;
    binForwardPlusLightsInto(result, scratch, points, pointCount, spots, spotCount,
                             viewMatrix, projMatrix, width, height, nearPlane, farPlane,
                             flipY, enabled, stats);
    return result;
}
