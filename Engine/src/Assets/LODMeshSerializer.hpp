#pragma once

#include "EngineExport.h"
#include "LODGenerator.hpp"
#include <string>

namespace Assets {

class ENGINE_API LODMeshSerializer {
public:
    static bool save(const LODMeshData& lod_data, const std::string& filepath);
    static bool load(LODMeshData& lod_data, const std::string& filepath);
};

} // namespace Assets
