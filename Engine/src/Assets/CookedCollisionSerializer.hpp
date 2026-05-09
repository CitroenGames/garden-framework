#pragma once

#include "AssetMetadata.hpp"
#include "EngineExport.h"
#include "LODGenerator.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <string>

namespace Assets {

class ENGINE_API CookedCollisionSerializer {
public:
    static constexpr uint32_t FORMAT_VERSION = 1;

    static std::string defaultFileNameForAsset(const std::string& asset_path);
    static std::string defaultPathForAsset(const std::string& asset_path);

    static bool cookMeshToFile(const LODMeshData& mesh_data,
                               const std::string& output_path,
                               uint64_t source_hash,
                               uint64_t source_file_size,
                               AssetMetadata::CollisionInfo* out_info = nullptr,
                               std::string* out_error = nullptr);

    static bool loadShape(const std::string& path,
                          uint64_t expected_source_hash,
                          uint64_t expected_source_file_size,
                          JPH::ShapeRefC& out_shape,
                          std::string* out_error = nullptr);
};

} // namespace Assets
