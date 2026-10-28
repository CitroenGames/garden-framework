#pragma once

#include "CombatProtocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>

// Authoritative, game-module combat feed.  This contains value data only: a
// later disconnect, renamed player, or removed pawn cannot change recorded
// attribution.  Presentation resolves current display names separately.
namespace FpsCombat {

constexpr size_t kCombatFeedCapacity = 32;
constexpr uint8_t kCombatFeedKnownFlags = 1u << 0; // FpsDeathEventSelfInflicted

struct ScoreChange {
    int8_t killer_kills = 0;
    int8_t victim_deaths = 0;
    bool match_kill_recorded = false;

    bool any() const { return killer_kills != 0 || victim_deaths != 0 || match_kill_recorded; }
};

struct CombatFeedEvent {
    uint16_t killer_client_id = 0;
    uint16_t victim_client_id = 0;
    uint8_t weapon_type = 0;
    uint8_t flags = 0;
    uint32_t death_sequence = 0;
    ScoreChange score_change{};
};

struct CombatFeedRecord {
    CombatFeedEvent event{};
    // Total order assigned by the server when two deaths resolve in one tick.
    uint64_t publication_sequence = 0;
};

// A single world-owned component holds the recent bounded feed.  It has no
// wallet, inventory, names, or entity references, so it is safe to publish to
// all connected clients once the UI/network ownership contract is extended.
struct CombatFeedComponent {
    std::array<CombatFeedRecord, kCombatFeedCapacity> records{};
    size_t first = 0;
    size_t count = 0;
    uint64_t next_publication_sequence = 1;
};

enum class CombatFeedRecordResult : uint8_t {
    Recorded,
    InvalidVictim,
    InvalidFlags,
    InvalidSelfAttribution,
};

inline bool isValidCombatFeedEvent(const CombatFeedEvent& event)
{
    if (event.victim_client_id == 0)
        return false;
    if ((event.flags & ~kCombatFeedKnownFlags) != 0)
        return false;

    const bool self_inflicted = (event.flags & kCombatFeedKnownFlags) != 0;
    return self_inflicted == (event.killer_client_id == event.victim_client_id);
}

inline CombatFeedComponent& combatFeed(entt::registry& registry)
{
    auto view = registry.view<CombatFeedComponent>();
    if (view.begin() != view.end())
        return view.get<CombatFeedComponent>(*view.begin());

    const entt::entity feed_entity = registry.create();
    return registry.emplace<CombatFeedComponent>(feed_entity);
}

inline CombatFeedRecordResult recordCombatFeedEvent(
    entt::registry& registry,
    const CombatFeedEvent& event)
{
    if (event.victim_client_id == 0)
        return CombatFeedRecordResult::InvalidVictim;
    if ((event.flags & ~kCombatFeedKnownFlags) != 0)
        return CombatFeedRecordResult::InvalidFlags;
    if (!isValidCombatFeedEvent(event))
        return CombatFeedRecordResult::InvalidSelfAttribution;

    CombatFeedComponent& feed = combatFeed(registry);
    const size_t write_index = (feed.first + feed.count) % kCombatFeedCapacity;
    CombatFeedRecord& record = feed.records[write_index];
    record.event = event;
    record.publication_sequence = feed.next_publication_sequence++;

    if (feed.count < kCombatFeedCapacity) {
        ++feed.count;
    } else {
        feed.first = (feed.first + 1) % kCombatFeedCapacity;
    }
    return CombatFeedRecordResult::Recorded;
}

inline const CombatFeedRecord* combatFeedRecordAt(const CombatFeedComponent& feed, size_t index)
{
    if (index >= feed.count)
        return nullptr;
    return &feed.records[(feed.first + index) % kCombatFeedCapacity];
}

inline CombatEventMessage makeCombatEventMessage(const CombatFeedRecord& record)
{
    const CombatFeedEvent& event = record.event;
    return {
        static_cast<uint8_t>(CombatEventMessageType::COMBAT_EVENT),
        event.killer_client_id,
        event.victim_client_id,
        event.weapon_type,
        event.flags,
        event.death_sequence,
        record.publication_sequence,
        event.score_change.killer_kills,
        event.score_change.victim_deaths,
        static_cast<uint8_t>(event.score_change.match_kill_recorded ? 1u : 0u),
    };
}

// This is a client-world/session-scoped cache, not a scoreboard.  It retains
// the latest accepted sequence per bounded victim window so duplicate and old
// network events cannot produce a second local death, kill, or feed row.
constexpr size_t kClientCombatEventIdentityCapacity = 32;

struct ClientCombatEventIdentity
{
    uint16_t victim_client_id = 0;
    uint32_t death_sequence = 0;
};

enum class ClientCombatEventAcceptResult : uint8_t {
    Accepted,
    Duplicate,
    Older,
    Invalid,
};

class ClientCombatEventIdentityCache
{
public:
    ClientCombatEventAcceptResult accept(const CombatEventMessage& message)
    {
        if (!isValidCombatEvent(message))
            return ClientCombatEventAcceptResult::Invalid;

        for (size_t i = 0; i < m_count; ++i) {
            ClientCombatEventIdentity& entry = m_entries[(m_first + i) % kClientCombatEventIdentityCapacity];
            if (entry.victim_client_id != message.victim_client_id)
                continue;
            if (entry.death_sequence == message.death_sequence)
                return ClientCombatEventAcceptResult::Duplicate;
            if (message.death_sequence < entry.death_sequence)
                return ClientCombatEventAcceptResult::Older;
            entry.death_sequence = message.death_sequence;
            return ClientCombatEventAcceptResult::Accepted;
        }

        const size_t write_index = (m_first + m_count) % kClientCombatEventIdentityCapacity;
        m_entries[write_index] = {message.victim_client_id, message.death_sequence};
        if (m_count < kClientCombatEventIdentityCapacity)
            ++m_count;
        else
            m_first = (m_first + 1) % kClientCombatEventIdentityCapacity;
        return ClientCombatEventAcceptResult::Accepted;
    }

    void clear()
    {
        m_first = 0;
        m_count = 0;
    }

    size_t count() const { return m_count; }

private:
    std::array<ClientCombatEventIdentity, kClientCombatEventIdentityCapacity> m_entries{};
    size_t m_first = 0;
    size_t m_count = 0;
};

inline void clearCombatFeed(entt::registry& registry)
{
    auto view = registry.view<CombatFeedComponent>();
    for (const entt::entity entity : view) {
        CombatFeedComponent& feed = view.get<CombatFeedComponent>(entity);
        feed.first = 0;
        feed.count = 0;
        // Keep the global sequence monotonic across rounds; a client can use
        // it to reject an old replay after the round-clear boundary.
    }
}

} // namespace FpsCombat
