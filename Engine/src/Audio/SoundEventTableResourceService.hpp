#pragma once

#include "Audio/SoundEventTable.hpp"
#include "EngineExport.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

// A persisted sound-event table identity. The current runtime contract accepts
// only authored .sound.json documents; C++ tables and other JSON are never
// fallback inputs for this service.
struct SoundEventTableResourceKey
{
    std::string path;

    bool operator==(const SoundEventTableResourceKey& other) const
    {
        return path == other.path;
    }
};

enum class SoundEventTableResourceState : uint8_t
{
    Unrequested,
    Loading,
    Ready,
    Failed
};

struct SoundEventTableResourceError
{
    std::string code;
    std::string message;
};

struct SoundEventTableLoadEntry;

class SoundEventTableLoadTicket
{
    friend class SoundEventTableResourceService;
public:
    SoundEventTableLoadTicket() = default;
    bool valid() const { return static_cast<bool>(m_entry); }

private:
    explicit SoundEventTableLoadTicket(std::shared_ptr<SoundEventTableLoadEntry> entry)
        : m_entry(std::move(entry)) {}
    std::shared_ptr<SoundEventTableLoadEntry> m_entry;
};

// Device-free immutable table loading. This intentionally stays outside
// AssetData/IAssetLoader because the table is structured gameplay metadata,
// not a GPU or mixer payload. Mutable voices remain in AudioEventSystem.
class ENGINE_API SoundEventTableResourceService final
{
public:
    static SoundEventTableResourceService& get();

    SoundEventTableLoadTicket request(const SoundEventTableResourceKey& key);
    std::shared_ptr<const SoundEventTable> tryGetReady(const SoundEventTableLoadTicket& ticket) const;
    std::optional<SoundEventTableResourceError> getError(const SoundEventTableLoadTicket& ticket) const;
    SoundEventTableResourceState getState(const SoundEventTableLoadTicket& ticket) const;

    // Removes one canonical cache entry. Existing tickets become a named
    // unloaded failure after a pending read finishes, while copied immutable
    // payloads remain valid through normal shared ownership.
    bool unload(const SoundEventTableResourceKey& key);

    // Publish completed validated documents only at a caller-owned safe
    // boundary. No device, renderer, or mixer is touched by this service.
    void update();
    void shutdown();

private:
    SoundEventTableResourceService() = default;
    ~SoundEventTableResourceService();
    SoundEventTableResourceService(const SoundEventTableResourceService&) = delete;
    SoundEventTableResourceService& operator=(const SoundEventTableResourceService&) = delete;

    std::string canonicalDocumentPath(const SoundEventTableResourceKey& key,
                                      SoundEventTableResourceError& error) const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<SoundEventTableLoadEntry>> m_entries;
};
