#include "Audio/SoundEventTableResourceService.hpp"

#include "Assets/AssetManager.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace
{
struct LoadResult
{
    std::shared_ptr<const SoundEventTable> resource;
    std::optional<SoundEventTableResourceError> error;
};

LoadResult loadDocument(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        return {{}, SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_READ",
            "could not open sound-event document: " + path}};

    const std::string document((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof())
        return {{}, SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_READ",
            "could not read sound-event document: " + path}};

    SoundEventTable table;
    std::string table_error;
    if (!SoundEventTable::deserialize(document, table, &table_error))
        return {{}, SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_INVALID",
            "sound-event document '" + path + "' is invalid: " + table_error}};

    return {std::make_shared<const SoundEventTable>(std::move(table)), std::nullopt};
}

bool hasSoundEventDocumentExtension(const fs::path& path)
{
    std::string name = path.filename().generic_string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    constexpr std::string_view extension = ".sound.json";
    return name.size() > extension.size() &&
        name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
}
} // namespace

struct SoundEventTableLoadEntry
{
    SoundEventTableResourceKey key;
    std::string canonical_path;
    SoundEventTableResourceState state = SoundEventTableResourceState::Unrequested;
    std::shared_ptr<const SoundEventTable> resource;
    std::optional<SoundEventTableResourceError> error;
    std::future<LoadResult> future;
    bool unloaded = false;
};

SoundEventTableResourceService& SoundEventTableResourceService::get()
{
    static SoundEventTableResourceService service;
    return service;
}

SoundEventTableResourceService::~SoundEventTableResourceService()
{
    shutdown();
}

std::string SoundEventTableResourceService::canonicalDocumentPath(
    const SoundEventTableResourceKey& key, SoundEventTableResourceError& error) const
{
    if (key.path.empty())
    {
        error = {"SOUND_EVENT_DOCUMENT_PATH", "sound-event document path is empty"};
        return {};
    }

    const std::string resolved = Assets::AssetManager::get().resolveAssetPath(key.path);
    if (resolved.empty())
    {
        error = {"SOUND_EVENT_DOCUMENT_PATH",
                 "sound-event document path escapes the configured asset root: " + key.path};
        return {};
    }

    fs::path document_path(resolved);
    if (!hasSoundEventDocumentExtension(document_path))
    {
        error = {"SOUND_EVENT_DOCUMENT_EXTENSION",
                 "sound-event resource must name a current .sound.json document: " + key.path};
        return {};
    }

    std::error_code ec;
    document_path = fs::absolute(document_path, ec);
    if (ec) document_path = fs::path(resolved);
    fs::path normalized = fs::weakly_canonical(document_path, ec);
    if (ec) normalized = document_path.lexically_normal();
    return normalized.generic_string();
}

SoundEventTableLoadTicket SoundEventTableResourceService::request(
    const SoundEventTableResourceKey& key)
{
    SoundEventTableResourceError path_error;
    const std::string canonical_path = canonicalDocumentPath(key, path_error);
    const std::string cache_key = canonical_path.empty() ? "!" + key.path : canonical_path;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!canonical_path.empty())
    {
        const auto existing = m_entries.find(cache_key);
        if (existing != m_entries.end())
            return SoundEventTableLoadTicket(existing->second);
    }

    auto entry = std::make_shared<SoundEventTableLoadEntry>();
    entry->key = key;
    entry->canonical_path = canonical_path;
    if (canonical_path.empty())
    {
        entry->state = SoundEventTableResourceState::Failed;
        entry->error = std::move(path_error);
    }
    else
    {
        entry->state = SoundEventTableResourceState::Loading;
        entry->future = std::async(std::launch::async, [canonical_path] {
            return loadDocument(canonical_path);
        });
    }
    m_entries.emplace(cache_key, entry);
    return SoundEventTableLoadTicket(std::move(entry));
}

std::shared_ptr<const SoundEventTable> SoundEventTableResourceService::tryGetReady(
    const SoundEventTableLoadTicket& ticket) const
{
    if (!ticket.m_entry) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->state == SoundEventTableResourceState::Ready
        ? ticket.m_entry->resource : nullptr;
}

std::optional<SoundEventTableResourceError> SoundEventTableResourceService::getError(
    const SoundEventTableLoadTicket& ticket) const
{
    if (!ticket.m_entry) return std::nullopt;
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->error;
}

SoundEventTableResourceState SoundEventTableResourceService::getState(
    const SoundEventTableLoadTicket& ticket) const
{
    if (!ticket.m_entry) return SoundEventTableResourceState::Unrequested;
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.m_entry->state;
}

bool SoundEventTableResourceService::unload(const SoundEventTableResourceKey& key)
{
    SoundEventTableResourceError path_error;
    const std::string canonical_path = canonicalDocumentPath(key, path_error);
    if (canonical_path.empty()) return false;

    std::shared_ptr<SoundEventTableLoadEntry> entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(canonical_path);
        if (found == m_entries.end()) return false;
        entry = found->second;
        entry->unloaded = true;
        m_entries.erase(found);
    }

    if (entry->future.valid()) entry->future.wait();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        entry->resource.reset();
        entry->error = SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_UNLOADED",
            "sound-event document was unloaded: " + canonical_path};
        entry->state = SoundEventTableResourceState::Failed;
    }
    return true;
}

void SoundEventTableResourceService::update()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [_, entry] : m_entries)
    {
        if (entry->state != SoundEventTableResourceState::Loading ||
            entry->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;

        LoadResult result = entry->future.get();
        if (entry->unloaded)
        {
            entry->resource.reset();
            entry->error = SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_UNLOADED",
                "sound-event document was unloaded: " + entry->canonical_path};
            entry->state = SoundEventTableResourceState::Failed;
        }
        else if (result.resource)
        {
            entry->resource = std::move(result.resource);
            entry->error.reset();
            entry->state = SoundEventTableResourceState::Ready;
        }
        else
        {
            entry->error = std::move(result.error);
            entry->state = SoundEventTableResourceState::Failed;
        }
    }
}

void SoundEventTableResourceService::shutdown()
{
    std::unordered_map<std::string, std::shared_ptr<SoundEventTableLoadEntry>> entries;
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
        entry->error = SoundEventTableResourceError{"SOUND_EVENT_DOCUMENT_UNLOADED",
            "sound-event resource service shut down"};
        entry->state = SoundEventTableResourceState::Failed;
    }
}
