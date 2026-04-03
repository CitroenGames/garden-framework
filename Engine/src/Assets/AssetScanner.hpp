#pragma once

#include "AssetMetadata.hpp"
#include <string>
#include <vector>
#include <mutex>

namespace Assets {

enum class AssetScanStatus {
    UpToDate,
    NeedsScan,
    NeedsUpdate,
    Processing,
    Error
};

struct ScannedAsset {
    std::string source_path;
    AssetScanStatus status = AssetScanStatus::NeedsScan;
    AssetMetadata metadata;
    std::string error_message;
};

class AssetScanner {
public:
    void scanDirectory(const std::string& root_dir);
    bool processAsset(const std::string& asset_path);
    bool processAssetWithConfig(const std::string& asset_path,
                                const AssetMetadata::LODConfig& config,
                                const std::vector<float>& screen_thresholds);
    AssetScanStatus checkAsset(const std::string& asset_path);
    const std::vector<ScannedAsset>& getScannedAssets() const;
    void processAllPending();
    bool regenerateAsset(const std::string& asset_path);

    void setLODConfig(const AssetMetadata::LODConfig& config) { m_default_lod_config = config; }
    const AssetMetadata::LODConfig& getLODConfig() const { return m_default_lod_config; }

private:
    std::vector<ScannedAsset> m_scanned_assets;
    AssetMetadata::LODConfig m_default_lod_config;
    mutable std::mutex m_mutex;

    bool isMeshFile(const std::string& path) const;
    std::string getBaseName(const std::string& path) const;
    std::string getDirectory(const std::string& path) const;
    std::string generateTimestamp() const;
};

} // namespace Assets
