#include "LODGenerator.hpp"
#include "meshoptimizer.h"
#include <cstdio>
#include <cstring>

namespace Assets {

LODGenerationResult LODGenerator::generate(const LODGenerationInput& input)
{
    LODGenerationResult result;

    if (!input.vertices || input.vertex_count == 0)
    {
        result.error_message = "No vertex data provided";
        return result;
    }

    const size_t vertex_size = sizeof(vertex);
    const size_t position_stride = vertex_size; // vx,vy,vz are at offset 0

    // Step 1: Build indexed mesh (deduplicate vertices if no index buffer provided)
    std::vector<vertex> base_vertices;
    std::vector<uint32_t> base_indices;

    if (input.indices && input.index_count > 0)
    {
        // Already indexed — copy as-is
        base_vertices.assign(input.vertices, input.vertices + input.vertex_count);
        base_indices.assign(input.indices, input.indices + input.index_count);
    }
    else
    {
        // Generate index buffer via vertex deduplication
        size_t total_verts = input.vertex_count;
        std::vector<unsigned int> remap(total_verts);

        size_t unique_count = meshopt_generateVertexRemap(
            remap.data(), nullptr, total_verts,
            input.vertices, total_verts, vertex_size
        );

        base_vertices.resize(unique_count);
        base_indices.resize(total_verts);

        meshopt_remapVertexBuffer(base_vertices.data(), input.vertices, total_verts, vertex_size, remap.data());
        meshopt_remapIndexBuffer(base_indices.data(), nullptr, total_verts, remap.data());

        printf("LODGenerator: Deduplicated %zu -> %zu vertices\n", total_verts, unique_count);
    }

    // Step 2: Optimize LOD0 (vertex cache, overdraw, vertex fetch)
    {
        std::vector<uint32_t> optimized_indices(base_indices.size());

        meshopt_optimizeVertexCache(
            optimized_indices.data(), base_indices.data(),
            base_indices.size(), base_vertices.size()
        );

        meshopt_optimizeOverdraw(
            base_indices.data(), optimized_indices.data(),
            optimized_indices.size(),
            &base_vertices[0].vx, base_vertices.size(), position_stride,
            1.05f // threshold
        );

        std::vector<vertex> fetch_optimized(base_vertices.size());
        meshopt_optimizeVertexFetch(
            fetch_optimized.data(), base_indices.data(),
            base_indices.size(),
            base_vertices.data(), base_vertices.size(), vertex_size
        );
        base_vertices = std::move(fetch_optimized);
    }

    // Store LOD0 (optimized original)
    {
        LODMeshData lod0;
        lod0.vertices = base_vertices;
        lod0.indices = base_indices;
        lod0.achieved_error = 0.0f;
        lod0.achieved_ratio = 1.0f;
        result.lod_meshes.push_back(std::move(lod0));
    }

    // Step 3: Generate simplified LOD levels
    float scale = meshopt_simplifyScale(&base_vertices[0].vx, base_vertices.size(), position_stride);

    for (int i = 1; i < input.config.max_lod_levels; ++i)
    {
        if (i >= static_cast<int>(input.config.target_ratios.size()))
            break;

        float target_ratio = input.config.target_ratios[i];
        size_t target_index_count = static_cast<size_t>(base_indices.size() * target_ratio);
        // Ensure at least 3 indices (one triangle)
        if (target_index_count < 3)
            target_index_count = 3;

        float target_error = input.config.target_error_threshold;
        float result_error = 0.0f;

        std::vector<uint32_t> simplified_indices(base_indices.size());

        unsigned int options = 0;
        if (input.config.lock_borders)
            options |= meshopt_SimplifyLockBorder;

        size_t simplified_count = meshopt_simplify(
            simplified_indices.data(),
            base_indices.data(), base_indices.size(),
            &base_vertices[0].vx, base_vertices.size(), position_stride,
            target_index_count, target_error, options, &result_error
        );

        if (simplified_count == 0)
        {
            printf("LODGenerator: LOD%d simplification produced 0 indices, stopping\n", i);
            break;
        }

        simplified_indices.resize(simplified_count);

        // Compact the vertex buffer for this LOD level
        // Build a remap from old indices to new compacted indices
        std::vector<uint32_t> vertex_remap(base_vertices.size(), UINT32_MAX);
        std::vector<vertex> compact_vertices;
        compact_vertices.reserve(simplified_count); // upper bound

        for (size_t j = 0; j < simplified_count; ++j)
        {
            uint32_t old_idx = simplified_indices[j];
            if (vertex_remap[old_idx] == UINT32_MAX)
            {
                vertex_remap[old_idx] = static_cast<uint32_t>(compact_vertices.size());
                compact_vertices.push_back(base_vertices[old_idx]);
            }
            simplified_indices[j] = vertex_remap[old_idx];
        }

        float achieved_ratio = static_cast<float>(simplified_count) / static_cast<float>(base_indices.size());

        LODMeshData lod;
        lod.vertices = std::move(compact_vertices);
        lod.indices = std::move(simplified_indices);
        lod.achieved_error = result_error * scale;
        lod.achieved_ratio = achieved_ratio;

        printf("LODGenerator: LOD%d - %zu indices (%zu tris), ratio=%.2f, error=%.6f\n",
               i, lod.indices.size(), lod.indices.size() / 3, achieved_ratio, result_error * scale);

        result.lod_meshes.push_back(std::move(lod));
    }

    result.success = true;
    printf("LODGenerator: Generated %zu LOD levels\n", result.lod_meshes.size());
    return result;
}

} // namespace Assets
