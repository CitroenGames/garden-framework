#pragma once

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
