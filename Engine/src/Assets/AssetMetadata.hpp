#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace Assets {

struct LODLevelInfo {
    int level = 0;
    float target_ratio = 1.0f;
    float target_error = 0.0f;
    float screen_threshold = 0.0f;
    size_t vertex_count = 0;
    size_t index_count = 0;
    size_t triangle_count = 0;
    std::string file_path; // relative path to .lodbin (empty for LOD0)
};

struct AssetMetadata {
    int version = 1;

    // Source file info
    std::string source_path;
    uint64_t source_hash = 0;
    uint64_t source_file_size = 0;

    // Mesh statistics (LOD0)
    size_t vertex_count = 0;
    size_t index_count = 0;
    size_t triangle_count = 0;
    size_t submesh_count = 0;

    // Bounds
    glm::vec3 aabb_min{0.0f};
    glm::vec3 aabb_max{0.0f};

    // LOD chain
    bool lod_enabled = true;
    std::vector<LODLevelInfo> lod_levels;

    // LOD generation config (stored so we can detect config changes)
    struct LODConfig {
        int max_lod_levels = 4;
        std::vector<float> target_ratios = {1.0f, 0.5f, 0.25f, 0.1f};
        float target_error_threshold = 0.01f;
        bool lock_borders = false;
    };
    LODConfig lod_config;

    std::string generated_at;
};

} // namespace Assets
