#pragma once

#include <cstdint>

namespace Assets {

static constexpr uint32_t CMESH_MAGIC   = 0x434D5348; // "CMSH"
static constexpr uint32_t CMESH_VERSION = 1;

enum CmeshFlags : uint32_t {
    CMESH_FLAG_HAS_INDICES   = 1 << 0,
    CMESH_FLAG_HAS_LODS      = 1 << 1,
    CMESH_FLAG_HAS_MATERIALS = 1 << 2
};

struct CmeshHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t submesh_count;
    uint32_t lod_count;          // includes LOD0
    uint32_t material_ref_count;
    float    aabb_min[3];
    float    aabb_max[3];
    uint64_t source_hash;        // FNV-1a of source file for incremental builds
};

} // namespace Assets
