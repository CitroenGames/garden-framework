#pragma once

#include "AssetTypes.hpp"
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

class IRenderAPI;

namespace Assets {

struct LoadResult {
    bool success = false;
    std::string error_message;
    AssetData data;

    std::vector<std::string> referenced_assets;
};

struct LoadContext {
    IRenderAPI* render_api = nullptr;
    std::string base_path;
    bool verbose_logging = false;
};

class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;

    virtual AssetType getAssetType() const = 0;

    virtual std::vector<std::string> getSupportedExtensions() const = 0;

    virtual bool canLoad(const std::string& path) const {
        auto extensions = getSupportedExtensions();
        std::string lower_path = path;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

        for (const auto& ext : extensions) {
            if (lower_path.size() >= ext.size() &&
                lower_path.compare(lower_path.size() - ext.size(), ext.size(), ext) == 0) {
                return true;
            }
        }
        return false;
    }

    virtual LoadResult loadFromFile(const std::string& path,
                                   const LoadContext& context) = 0;

    virtual bool uploadToGPU(AssetData& data, IRenderAPI* render_api) = 0;

    virtual float estimateLoadTime(const std::string& path) const { return 1.0f; }
};

} // namespace Assets
