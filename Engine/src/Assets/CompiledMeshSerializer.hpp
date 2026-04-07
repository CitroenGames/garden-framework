#pragma once

#include "EngineExport.h"
#include "CompiledMeshFormat.hpp"
#include "LODGenerator.hpp"      // LODSubmeshRange, vertex
#include "Utils/GltfMaterialLoader.hpp" // TextureType
#include <string>
#include <vector>

namespace Assets {

struct CompiledMeshData {
    CmeshHeader header{};

    // Submeshes (one per primitive/material slot)
    struct Submesh {
        uint32_t material_index = 0;
        std::string name;
    };
    std::vector<Submesh> submeshes;

    // Material references stored in the file so runtime can resolve textures
    struct MaterialRef {
        std::string name;
        float base_color_factor[4] = {1, 1, 1, 1};
        float metallic_factor  = 1.0f;
        float roughness_factor = 1.0f;
        uint8_t alpha_mode     = 0;   // 0=OPAQUE, 1=MASK, 2=BLEND
        float alpha_cutoff     = 0.5f;
        bool  double_sided     = false;

        struct TextureRef {
            uint8_t     type = 0; // TextureType cast to uint8
            std::string path;     // relative, uses .ctex when compiled
        };
        std::vector<TextureRef> textures;
    };
    std::vector<MaterialRef> material_refs;

    // LOD levels (LOD0 = optimised original, LOD1+ = simplified)
    struct LODLevel {
        std::vector<vertex>   vertices;
        std::vector<uint32_t> indices;
        float screen_threshold = 0.0f;
        float achieved_error   = 0.0f;
        float achieved_ratio   = 1.0f;
        std::vector<LODSubmeshRange> submesh_ranges;
    };
    std::vector<LODLevel> lod_levels;
};

class ENGINE_API CompiledMeshSerializer {
public:
    static bool save(const CompiledMeshData& data, const std::string& filepath);
    static bool load(CompiledMeshData& data, const std::string& filepath);
    static bool loadHeader(CmeshHeader& header, const std::string& filepath);
};

} // namespace Assets
