#include "MeshChunker.hpp"

#include "meshoptimizer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace Assets {
namespace {

struct VertexHash {
    size_t operator()(const vertex& v) const {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(&v);
        size_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < sizeof(vertex); ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

struct VertexEqual {
    bool operator()(const vertex& a, const vertex& b) const {
        return std::memcmp(&a, &b, sizeof(vertex)) == 0;
    }
};

struct BoundsBuilder {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
    bool valid = false;

    void include(const vertex& v) {
        glm::vec3 p(v.vx, v.vy, v.vz);
        min = glm::min(min, p);
        max = glm::max(max, p);
        valid = true;
    }
};

size_t resolveChunkTriangleTarget(size_t total_triangles, const MeshChunkConfig& config) {
    size_t target = std::max<size_t>(config.target_triangles, 1);
    if (config.max_chunks > 0 && total_triangles > target * config.max_chunks)
        target = (total_triangles + config.max_chunks - 1) / config.max_chunks;
    return std::max<size_t>(target, 1);
}

void copyRangePayload(MaterialRange& dst, const MaterialRange& src) {
    dst.texture = src.texture;
    dst.material_name = src.material_name;
    dst.alpha_mode = src.alpha_mode;
    dst.alpha_cutoff = src.alpha_cutoff;
    dst.double_sided = src.double_sided;
    dst.metallic_factor = src.metallic_factor;
    dst.roughness_factor = src.roughness_factor;
    dst.emissive_factor = src.emissive_factor;
    dst.base_color_factor = src.base_color_factor;
    dst.metallic_roughness_texture = src.metallic_roughness_texture;
    dst.normal_texture = src.normal_texture;
    dst.occlusion_texture = src.occlusion_texture;
    dst.emissive_texture = src.emissive_texture;
    dst.source_range = src.source_range;
}

uint32_t appendVertex(const vertex& v,
                      std::vector<vertex>& vertices,
                      std::unordered_map<vertex, uint32_t, VertexHash, VertexEqual>& remap) {
    auto it = remap.find(v);
    if (it != remap.end())
        return it->second;

    uint32_t index = static_cast<uint32_t>(vertices.size());
    vertices.push_back(v);
    remap.emplace(vertices.back(), index);
    return index;
}

bool shouldSplitSubmesh(size_t submesh_id, const std::vector<bool>* split_submesh) {
    if (!split_submesh)
        return true;
    if (submesh_id >= split_submesh->size())
        return true;
    return (*split_submesh)[submesh_id];
}

} // namespace

ChunkedTriangleMesh MeshChunker::buildChunkedIndexedMesh(
    const vertex* source_vertices,
    size_t source_vertex_count,
    const std::vector<MaterialRange>& source_ranges,
    const MeshChunkConfig& config) {

    ChunkedTriangleMesh result;
    if (!config.enabled || !source_vertices || source_vertex_count < 3)
        return result;

    std::vector<MaterialRange> ranges;
    if (source_ranges.empty()) {
        MaterialRange full(0, source_vertex_count, INVALID_TEXTURE, "");
        full.source_range = 0;
        ranges.push_back(full);
    } else {
        ranges = source_ranges;
        for (size_t i = 0; i < ranges.size(); ++i) {
            if (ranges[i].source_range == std::numeric_limits<size_t>::max())
                ranges[i].source_range = i;
        }
    }

    size_t total_triangles = 0;
    for (const auto& range : ranges) {
        if (range.start_vertex >= source_vertex_count)
            continue;
        size_t count = std::min(range.vertex_count, source_vertex_count - range.start_vertex);
        total_triangles += count / 3;
    }
    if (total_triangles == 0)
        return result;

    const size_t target_triangles = resolveChunkTriangleTarget(total_triangles, config);
    const size_t target_indices = target_triangles * 3;

    std::unordered_map<vertex, uint32_t, VertexHash, VertexEqual> remap;
    remap.reserve(source_vertex_count);
    result.vertices.reserve(source_vertex_count);
    result.indices.reserve(total_triangles * 3);

    for (const auto& source_range : ranges) {
        if (source_range.start_vertex >= source_vertex_count)
            continue;

        size_t range_vertex_count = std::min(source_range.vertex_count, source_vertex_count - source_range.start_vertex);
        range_vertex_count -= range_vertex_count % 3;
        if (range_vertex_count == 0)
            continue;

        std::vector<uint32_t> local_indices(range_vertex_count);
        for (uint32_t i = 0; i < static_cast<uint32_t>(range_vertex_count); ++i)
            local_indices[i] = i;

        std::vector<uint32_t> sorted_indices(range_vertex_count);
        meshopt_spatialSortTriangles(
            sorted_indices.data(),
            local_indices.data(),
            range_vertex_count,
            &source_vertices[source_range.start_vertex].vx,
            range_vertex_count,
            sizeof(vertex));

        const bool split = !source_range.isAlphaBlend();
        const size_t chunk_indices = split ? target_indices : range_vertex_count;

        for (size_t start = 0; start < sorted_indices.size(); start += chunk_indices) {
            const size_t count = std::min(chunk_indices, sorted_indices.size() - start);
            if (count == 0)
                break;

            MaterialRange out_range;
            copyRangePayload(out_range, source_range);
            out_range.start_vertex = result.indices.size();
            out_range.vertex_count = count;

            BoundsBuilder bounds;
            for (size_t i = 0; i < count; ++i) {
                const uint32_t local_index = sorted_indices[start + i];
                if (local_index >= range_vertex_count)
                    continue;
                const vertex& v = source_vertices[source_range.start_vertex + local_index];
                bounds.include(v);
                result.indices.push_back(appendVertex(v, result.vertices, remap));
            }

            if (bounds.valid) {
                out_range.has_bounds = true;
                out_range.aabb_min = bounds.min;
                out_range.aabb_max = bounds.max;
            }
            result.material_ranges.push_back(out_range);
        }
    }

    return result;
}

LODMeshData MeshChunker::chunkLODMesh(
    const LODMeshData& lod,
    const MeshChunkConfig& config,
    const std::vector<bool>* split_submesh) {

    if (!config.enabled || lod.vertices.empty() || lod.indices.empty())
        return lod;

    std::vector<LODSubmeshRange> source_ranges = lod.submesh_ranges;
    if (source_ranges.empty()) {
        LODSubmeshRange full;
        full.start_index = 0;
        full.index_count = lod.indices.size();
        full.submesh_id = 0;
        source_ranges.push_back(full);
    }

    size_t total_triangles = 0;
    for (const auto& range : source_ranges) {
        if (range.start_index >= lod.indices.size())
            continue;
        size_t count = std::min(range.index_count, lod.indices.size() - range.start_index);
        total_triangles += count / 3;
    }
    if (total_triangles == 0)
        return lod;

    const size_t target_triangles = resolveChunkTriangleTarget(total_triangles, config);
    const size_t target_indices = target_triangles * 3;

    LODMeshData result;
    result.vertices = lod.vertices;
    result.achieved_error = lod.achieved_error;
    result.achieved_ratio = lod.achieved_ratio;
    result.indices.reserve(lod.indices.size());

    for (const auto& source_range : source_ranges) {
        if (source_range.start_index >= lod.indices.size())
            continue;

        size_t range_index_count = std::min(source_range.index_count, lod.indices.size() - source_range.start_index);
        range_index_count -= range_index_count % 3;
        if (range_index_count == 0)
            continue;

        std::vector<uint32_t> sorted_indices(range_index_count);
        meshopt_spatialSortTriangles(
            sorted_indices.data(),
            lod.indices.data() + source_range.start_index,
            range_index_count,
            &lod.vertices[0].vx,
            lod.vertices.size(),
            sizeof(vertex));

        const bool split = shouldSplitSubmesh(source_range.submesh_id, split_submesh);
        const size_t chunk_indices = split ? target_indices : range_index_count;

        for (size_t start = 0; start < sorted_indices.size(); start += chunk_indices) {
            const size_t count = std::min(chunk_indices, sorted_indices.size() - start);
            if (count == 0)
                break;

            LODSubmeshRange out_range;
            out_range.start_index = result.indices.size();
            out_range.index_count = count;
            out_range.submesh_id = source_range.submesh_id;

            BoundsBuilder bounds;
            for (size_t i = 0; i < count; ++i) {
                const uint32_t index = sorted_indices[start + i];
                if (index < result.vertices.size())
                    bounds.include(result.vertices[index]);
                result.indices.push_back(index);
            }

            if (bounds.valid) {
                out_range.has_bounds = true;
                out_range.aabb_min = bounds.min;
                out_range.aabb_max = bounds.max;
            }
            result.submesh_ranges.push_back(out_range);
        }
    }

    return result;
}

} // namespace Assets
