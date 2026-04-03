#pragma once

#include "AssetMetadata.hpp"
#include <string>

namespace Assets {

class AssetMetadataSerializer {
public:
    static bool save(const AssetMetadata& metadata, const std::string& meta_path);
    static bool load(AssetMetadata& metadata, const std::string& meta_path);
    static bool exists(const std::string& asset_path);
    static std::string getMetaPath(const std::string& asset_path);
};

} // namespace Assets
