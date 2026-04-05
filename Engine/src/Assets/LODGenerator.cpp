#include "LODGenerator.hpp"
#include "Utils/Log.hpp"
#include "meshoptimizer.h"
#include <cstring>

namespace Assets {

// Simplify a single submesh: deduplicate, optimize, and simplify at each LOD level.
// Returns one LODMeshData per LOD level (index 0 = optimized original, 1+ = simplified).
static std::vector<LODMeshData> simplifySubmesh(
    const vertex* vertices, size_t vertex_count,
    const AssetMetadata::LODConfig& config)
{
    std::vector<LODMeshData> results;
    if (!vertices || vertex_count == 0) return results;

    const size_t vertex_size = sizeof(vertex);
    const size_t position_stride = vertex_size;

    // Deduplicate vertices and generate index buffer
    std::vector<vertex> base_vertices;
    std::vector<uint32_t> base_indices;

    size_t total_verts = vertex_count;
    std::vector<unsigned int> remap(total_verts);

    size_t unique_count = meshopt_generateVertexRemap(
        remap.data(), nullptr, total_verts,
        vertices, total_verts, vertex_size
    );

    base_vertices.resize(unique_count);
    base_indices.resize(total_verts);

    meshopt_remapVertexBuffer(base_vertices.data(), vertices, total_verts, vertex_size, remap.data());
    meshopt_remapIndexBuffer(base_indices.data(), nullptr, total_verts, remap.data());

    // Optimize LOD0
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
            1.05f
        );
        std::vector<vertex> fetch_optimized(base_vertices.size());
        meshopt_optimizeVertexFetch(
            fetch_optimized.data(), base_indices.data(),
            base_indices.size(),
            base_vertices.data(), base_vertices.size(), vertex_size
        );
        base_vertices = std::move(fetch_optimized);
    }

    // LOD0
    {
        LODMeshData lod0;
        lod0.vertices = base_vertices;
        lod0.indices = base_indices;
        lod0.achieved_error = 0.0f;
        lod0.achieved_ratio = 1.0f;
        results.push_back(std::move(lod0));
    }

    // Simplified LOD levels
    float scale = meshopt_simplifyScale(&base_vertices[0].vx, base_vertices.size(), position_stride);

    for (int i = 1; i < config.max_lod_levels; ++i)
    {
        if (i >= static_cast<int>(config.target_ratios.size()))
            break;

        float target_ratio = config.target_ratios[i];
        size_t target_index_count = static_cast<size_t>(base_indices.size() * target_ratio);
        if (target_index_count < 3)
            target_index_count = 3;

        float target_error = config.target_error_threshold;
        float result_error = 0.0f;

        std::vector<uint32_t> simplified_indices(base_indices.size());

        unsigned int options = 0;
        if (config.lock_borders)
            options |= meshopt_SimplifyLockBorder;
        if (config.allow_attribute_collapse)
            options |= meshopt_SimplifyPermissive;
        if (config.prune_disconnected)
            options |= meshopt_SimplifyPrune;

        size_t simplified_count = meshopt_simplify(
            simplified_indices.data(),
            base_indices.data(), base_indices.size(),
            &base_vertices[0].vx, base_vertices.size(), position_stride,
            target_index_count, target_error, options, &result_error
        );

        if (simplified_count == 0)
            break;

        simplified_indices.resize(simplified_count);

        // Compact vertex buffer
        std::vector<uint32_t> vertex_remap(base_vertices.size(), UINT32_MAX);
        std::vector<vertex> compact_vertices;
        compact_vertices.reserve(simplified_count);

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
        results.push_back(std::move(lod));
    }

    return results;
}

LODGenerationResult LODGenerator::generate(const LODGenerationInput& input)
{
    LODGenerationResult result;

    if (!input.vertices || input.vertex_count == 0)
    {
        result.error_message = "No vertex data provided";
        return result;
    }

    const size_t vertex_size = sizeof(vertex);
    const size_t position_stride = vertex_size;

    // --- Per-submesh LOD generation ---
    if (!input.submeshes.empty())
    {
        LOG_ENGINE_INFO("LODGenerator: Per-submesh mode with {} submeshes", input.submeshes.size());

        // Simplify each submesh independently
        std::vector<std::vector<LODMeshData>> per_submesh_lods;
        per_submesh_lods.reserve(input.submeshes.size());

        int max_lod_levels = 1;
        for (size_t s = 0; s < input.submeshes.size(); ++s)
        {
            const auto& sub = input.submeshes[s];
            auto sub_lods = simplifySubmesh(
                input.vertices + sub.start_vertex, sub.vertex_count,
                input.config
            );
            if (static_cast<int>(sub_lods.size()) > max_lod_levels)
                max_lod_levels = static_cast<int>(sub_lods.size());
            per_submesh_lods.push_back(std::move(sub_lods));
        }

        // Concatenate submeshes for each LOD level
        for (int lod = 0; lod < max_lod_levels; ++lod)
        {
            LODMeshData combined;
            combined.achieved_error = 0.0f;
            combined.achieved_ratio = 0.0f;
            float total_base_indices = 0.0f;
            float total_simplified_indices = 0.0f;

            for (size_t s = 0; s < per_submesh_lods.size(); ++s)
            {
                const auto& sub_lods = per_submesh_lods[s];
                // Use the requested LOD level, or the highest available for this submesh
                int sub_lod = std::min(lod, static_cast<int>(sub_lods.size()) - 1);
                const auto& sub_data = sub_lods[sub_lod];

                LODSubmeshRange range;
                range.start_index = combined.indices.size();
                range.index_count = sub_data.indices.size();
                range.submesh_id = s;
                combined.submesh_ranges.push_back(range);

                // Adjust indices by current vertex offset
                uint32_t vertex_offset = static_cast<uint32_t>(combined.vertices.size());
                for (uint32_t idx : sub_data.indices)
                    combined.indices.push_back(idx + vertex_offset);

                combined.vertices.insert(combined.vertices.end(),
                    sub_data.vertices.begin(), sub_data.vertices.end());

                combined.achieved_error = std::max(combined.achieved_error, sub_data.achieved_error);

                // Track ratio from base LOD
                if (!sub_lods.empty())
                {
                    total_base_indices += static_cast<float>(sub_lods[0].indices.size());
                    total_simplified_indices += static_cast<float>(sub_data.indices.size());
                }
            }

            combined.achieved_ratio = (total_base_indices > 0.0f)
                ? total_simplified_indices / total_base_indices : 1.0f;

            LOG_ENGINE_INFO("LODGenerator: LOD{} combined - {} tris, {} verts, {} submeshes, ratio={:.2f}",
                            lod, combined.indices.size() / 3, combined.vertices.size(),
                            combined.submesh_ranges.size(), combined.achieved_ratio);

            result.lod_meshes.push_back(std::move(combined));
        }

        result.success = true;
        LOG_ENGINE_INFO("LODGenerator: Generated {} LOD levels (per-submesh)", result.lod_meshes.size());
        return result;
    }

    // --- Original single-mesh LOD generation (no submesh info) ---

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

        LOG_ENGINE_INFO("LODGenerator: Deduplicated {} -> {} vertices", total_verts, unique_count);
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

    LOG_ENGINE_INFO("LODGenerator: Config - error_threshold={:.6f}, lock_borders={}, collapse_seams={}, prune={}",
                    input.config.target_error_threshold,
                    input.config.lock_borders,
                    input.config.allow_attribute_collapse,
                    input.config.prune_disconnected);
    LOG_ENGINE_INFO("LODGenerator: Base mesh - {} verts, {} indices ({} tris), scale={:.6f}",
                    base_vertices.size(), base_indices.size(), base_indices.size() / 3, scale);

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
        if (input.config.allow_attribute_collapse)
            options |= meshopt_SimplifyPermissive;
        if (input.config.prune_disconnected)
            options |= meshopt_SimplifyPrune;

        LOG_ENGINE_INFO("LODGenerator: LOD{} - target ratio={:.0f}%, target indices={} (of {}), error limit={:.6f}, options=0x{:x}",
                        i, target_ratio * 100.0f, target_index_count, base_indices.size(), target_error, options);

        size_t simplified_count = meshopt_simplify(
            simplified_indices.data(),
            base_indices.data(), base_indices.size(),
            &base_vertices[0].vx, base_vertices.size(), position_stride,
            target_index_count, target_error, options, &result_error
        );

        float achieved_pct = 100.0f * static_cast<float>(simplified_count) / static_cast<float>(base_indices.size());

        LOG_ENGINE_INFO("LODGenerator: LOD{} - meshopt returned {} indices ({:.1f}% of base), error={:.6f} (relative), {:.6f} (absolute)",
                        i, simplified_count, achieved_pct, result_error, result_error * scale);

        if (simplified_count == 0)
        {
            LOG_ENGINE_WARN("LODGenerator: LOD{} produced 0 indices, stopping", i);
            break;
        }

        if (simplified_count >= base_indices.size())
        {
            LOG_ENGINE_WARN("LODGenerator: LOD{} - no reduction! Error budget ({:.6f}) is too tight for this mesh", i, target_error);
        }
        else if (achieved_pct > target_ratio * 100.0f + 5.0f)
        {
            LOG_ENGINE_WARN("LODGenerator: LOD{} - achieved {:.1f}% but target was {:.0f}%. Error threshold ({:.6f}) is limiting simplification",
                            i, achieved_pct, target_ratio * 100.0f, target_error);
            if (!input.config.allow_attribute_collapse)
                LOG_ENGINE_WARN("LODGenerator: LOD{} - try enabling 'Collapse Across Seams' for more aggressive reduction", i);
            else
                LOG_ENGINE_WARN("LODGenerator: LOD{} - try increasing the Quality (error threshold) value", i);
        }

        simplified_indices.resize(simplified_count);

        // Compact the vertex buffer for this LOD level
        std::vector<uint32_t> vertex_remap(base_vertices.size(), UINT32_MAX);
        std::vector<vertex> compact_vertices;
        compact_vertices.reserve(simplified_count);

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

        LOG_ENGINE_INFO("LODGenerator: LOD{} result - {} tris, {} verts, ratio={:.2f}, error={:.6f}",
                        i, lod.indices.size() / 3, lod.vertices.size(), achieved_ratio, lod.achieved_error);

        result.lod_meshes.push_back(std::move(lod));
    }

    result.success = true;
    LOG_ENGINE_INFO("LODGenerator: Generated {} LOD levels", result.lod_meshes.size());
    return result;
}

} // namespace Assets
