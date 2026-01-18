#pragma once

#include "AssetTypes.hpp"
#include <atomic>
#include <functional>
#include <future>
#include <optional>
#include <cstdint>

namespace Assets {

using AssetId = uint64_t;
constexpr AssetId INVALID_ASSET_ID = 0;

using LoadCallback = std::function<void(AssetId, bool success, const AssetData& data)>;
using ProgressCallback = std::function<void(AssetId, float progress, LoadState state)>;

class AssetManager;

class AssetHandle {
public:
    AssetHandle() = default;
    explicit AssetHandle(AssetId id, AssetManager* manager = nullptr);

    bool isValid() const { return m_id != INVALID_ASSET_ID; }
    AssetId getId() const { return m_id; }

    LoadState getState() const;
    float getProgress() const;
    bool isLoading() const;
    bool isReady() const;
    bool hasFailed() const;

    std::optional<AssetError> getError() const;

    AssetData getData() const;

    template<typename T>
    std::shared_ptr<T> getAs() const {
        AssetData data = getData();
        if (auto* ptr = std::get_if<std::shared_ptr<T>>(&data)) {
            return *ptr;
        }
        return nullptr;
    }

    void wait() const;
    bool waitFor(std::chrono::milliseconds timeout) const;

private:
    AssetId m_id = INVALID_ASSET_ID;
    AssetManager* m_manager = nullptr;
};

} // namespace Assets
