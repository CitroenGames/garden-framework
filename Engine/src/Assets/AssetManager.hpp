#pragma once

#include "EngineExport.h"
#include "AssetTypes.hpp"
#include "AssetHandle.hpp"
#include "IAssetLoader.hpp"
#include "Threading/JobSystem.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <vector>
#include <string>
#include <optional>

class IRenderAPI;

namespace Assets {

struct AssetState {
    AssetId id = INVALID_ASSET_ID;
    std::string path;
    AssetType type = AssetType::Unknown;
    LoadState state = LoadState::NotLoaded;
    float progress = 0.0f;

    AssetData data;
    std::optional<AssetError> error;

    std::promise<AssetData> promise;
    std::shared_future<AssetData> future;

    LoadCallback on_complete;
    ProgressCallback on_progress;

    Threading::JobHandle load_job = Threading::INVALID_JOB_HANDLE;
    Threading::JobHandle gpu_job = Threading::INVALID_JOB_HANDLE;

    AssetState() {
        future = promise.get_future().share();
    }
};

class ENGINE_API AssetManager {
    friend class AssetHandle;

public:
    static AssetManager& get();

    bool initialize(IRenderAPI* render_api);
    void shutdown();
    bool isInitialized() const { return m_initialized.load(std::memory_order_acquire); }

    // Asset root directory for resolving relative asset paths.
    // When set, resolveAssetPath() uses this as the base instead of CWD.
    void setAssetRoot(const std::string& root);
    const std::string& getAssetRoot() const { return m_asset_root; }

    // Resolve a relative asset path to an absolute path using the asset root.
    // If no asset root is set, returns the path as-is (CWD-relative).
    std::string resolveAssetPath(const std::string& relative_path) const;

    // Scan level files and collect all referenced asset paths (mesh, texture, collider).
    // Useful for packaging validation.
    static std::vector<std::string> collectLevelAssets(const std::string& levels_directory);

    void registerLoader(std::unique_ptr<IAssetLoader> loader);

    AssetHandle loadAsync(const std::string& path,
                         LoadPriority priority = LoadPriority::Normal,
                         LoadCallback on_complete = nullptr,
                         ProgressCallback on_progress = nullptr);

    AssetHandle loadSync(const std::string& path);

    std::vector<AssetHandle> loadBatch(const std::vector<std::string>& paths,
                                       LoadPriority priority = LoadPriority::Normal);

    LoadState getLoadState(AssetId id) const;
    float getProgress(AssetId id) const;
    AssetData getData(AssetId id) const;
    std::optional<AssetError> getError(AssetId id) const;

    bool isLoaded(const std::string& path) const;
    AssetHandle getLoadedAsset(const std::string& path) const;
    void unload(AssetId id);
    void unloadUnused();
    void clearCache();

    void update();

    size_t getLoadingCount() const;
    size_t getCachedCount() const;

private:
    AssetManager() = default;
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    IAssetLoader* findLoaderForPath(const std::string& path) const;
    void updateProgress(AssetId id, float progress, LoadState state);
    void completeLoad(AssetId id, bool success, const AssetData& data);
    void failLoad(AssetId id, const std::string& error);
    AssetState* getAssetState(AssetId id) const;

    IRenderAPI* m_render_api = nullptr;

    std::vector<std::unique_ptr<IAssetLoader>> m_loaders;

    std::unordered_map<AssetId, std::unique_ptr<AssetState>> m_assets;
    mutable std::shared_mutex m_assets_mutex;

    std::unordered_map<std::string, AssetId> m_path_to_id;
    mutable std::mutex m_path_mutex;

    std::atomic<AssetId> m_next_id{1};
    std::atomic<bool> m_initialized{false};

    std::string m_asset_root;  // Base directory for asset resolution
};

} // namespace Assets
