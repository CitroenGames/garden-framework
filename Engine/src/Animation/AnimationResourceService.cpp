#include "AnimationResourceService.hpp"

#include "Animation/CompiledAnimationSerializer.hpp"
#include "Assets/AssetManager.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <utility>

namespace fs = std::filesystem;

namespace
{
struct LoadResult
{
    std::shared_ptr<const AnimationResourceSet> resource;
    std::optional<AnimationResourceError> error;
};

LoadResult loadPackage(const std::string& path, uint32_t skin_index)
{
    CompiledAnimationData data;
    AnimationResourceError error;
    if (!CompiledAnimationSerializer::load(data, path, &error))
        return {{}, std::move(error)};

    for (const CompiledAnimationSkin& skin : data.skins)
        if (skin.skin_index == skin_index && skin.resource)
            return {skin.resource, std::nullopt};

    return {{}, AnimationResourceError{"ANIM_SKIN_NOT_FOUND",
        "CANM package '" + path + "' does not contain skin " + std::to_string(skin_index)}};
}
} // namespace

struct AnimationLoadEntry
{
    AnimationResourceKey key;
    std::string canonical_path;
    AnimationResourceState state = AnimationResourceState::Unrequested;
    std::shared_ptr<const AnimationResourceSet> resource;
    std::optional<AnimationResourceError> error;
    std::future<LoadResult> future;
    bool unloaded = false;
};

AnimationResourceService& AnimationResourceService::get()
{
    static AnimationResourceService service;
    return service;
}

AnimationResourceService::~AnimationResourceService()
{
    shutdown();
}

std::string AnimationResourceService::canonicalPackagePath(const AnimationResourceKey& key,
                                                            AnimationResourceError& error) const
{
    if (key.path.empty())
    {
        error = {"ANIM_PACKAGE_PATH", "animation resource path is empty"};
        return {};
    }

    std::string resolved = Assets::AssetManager::get().resolveAssetPath(key.path);
    if (resolved.empty())
    {
        error = {"ANIM_PACKAGE_PATH", "animation resource path escapes the configured asset root: " + key.path};
        return {};
    }
    fs::path package_path(resolved);
    if (package_path.extension() != ".canim")
    {
        error = {"ANIM_PACKAGE_EXTENSION", "animation resource must name a cooked .canim package: " + key.path};
        return {};
    }
    std::error_code ec;
    package_path = fs::absolute(package_path, ec);
    if (ec)
        package_path = fs::path(resolved);
    fs::path normalized = fs::weakly_canonical(package_path, ec);
    if (ec)
        normalized = package_path.lexically_normal();
    return normalized.generic_string();
}

AnimationLoadTicket AnimationResourceService::request(const AnimationResourceKey& key)
{
    AnimationResourceError path_error;
    const std::string canonical_path = canonicalPackagePath(key, path_error);
    // An invalid path still receives a stable per-authored-key failure. Do not
    // collapse distinct rejected paths into one diagnostic entry.
    const std::string cache_key = (canonical_path.empty() ? "!" + key.path : canonical_path) +
        "#" + std::to_string(key.skin_index);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!canonical_path.empty())
    {
        const auto existing = m_entries.find(cache_key);
        if (existing != m_entries.end())
            return AnimationLoadTicket(existing->second);
    }

    auto entry = std::make_shared<AnimationLoadEntry>();
    entry->key = key;
    entry->canonical_path = canonical_path;
    if (canonical_path.empty())
    {
        entry->state = AnimationResourceState::Failed;
        entry->error = std::move(path_error);
    }
    else
    {
        entry->state = AnimationResourceState::Loading;
        entry->future = std::async(std::launch::async, [canonical_path, skin_index = key.skin_index] {
            return loadPackage(canonical_path, skin_index);
        });
    }
    m_entries.emplace(cache_key, entry);
    return AnimationLoadTicket(std::move(entry));
}

std::shared_ptr<const AnimationResourceSet> AnimationResourceService::tryGetReady(const AnimationLoadTicket& ticket) const
{
    if (!ticket.m_entry) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->state == AnimationResourceState::Ready ? ticket.m_entry->resource : nullptr;
}

std::optional<AnimationResourceError> AnimationResourceService::getError(const AnimationLoadTicket& ticket) const
{
    if (!ticket.m_entry) return std::nullopt;
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->error;
}

AnimationResourceState AnimationResourceService::getState(const AnimationLoadTicket& ticket) const
{
    if (!ticket.m_entry) return AnimationResourceState::Unrequested;
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->state;
}

bool AnimationResourceService::unload(const AnimationResourceKey& key)
{
    AnimationResourceError path_error;
    const std::string canonical_path = canonicalPackagePath(key, path_error);
    if (canonical_path.empty()) return false;
    const std::string cache_key = canonical_path + "#" + std::to_string(key.skin_index);

    std::shared_ptr<AnimationLoadEntry> entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(cache_key);
        if (found == m_entries.end()) return false;
        entry = found->second;
        entry->unloaded = true;
        m_entries.erase(found);
    }

    if (entry->future.valid()) entry->future.wait();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        entry->resource.reset();
        entry->error = AnimationResourceError{"ANIM_RESOURCE_UNLOADED",
            "animation resource was unloaded: " + canonical_path};
        entry->state = AnimationResourceState::Failed;
    }
    return true;
}
void AnimationResourceService::update()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [_, entry] : m_entries)
    {
        if (entry->state != AnimationResourceState::Loading ||
            entry->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;

        LoadResult result = entry->future.get();
        if (entry->unloaded)
        {
            entry->resource.reset();
            entry->error = AnimationResourceError{"ANIM_RESOURCE_UNLOADED",
                "animation resource was unloaded: " + entry->canonical_path};
            entry->state = AnimationResourceState::Failed;
        }
        else if (result.resource)
        {
            entry->resource = std::move(result.resource);
            entry->error.reset();
            entry->state = AnimationResourceState::Ready;
        }
        else
        {
            entry->error = std::move(result.error);
            entry->state = AnimationResourceState::Failed;
        }
    }
}

void AnimationResourceService::shutdown()
{
    std::unordered_map<std::string, std::shared_ptr<AnimationLoadEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        entries.swap(m_entries);
        for (auto& [_, entry] : entries) entry->unloaded = true;
    }
    for (auto& [_, entry] : entries)
    {
        if (entry->future.valid()) entry->future.wait();
        std::lock_guard<std::mutex> lock(m_mutex);
        entry->resource.reset();
        entry->error = AnimationResourceError{"ANIM_RESOURCE_UNLOADED",
            "animation resource service shut down"};
        entry->state = AnimationResourceState::Failed;
    }
}
