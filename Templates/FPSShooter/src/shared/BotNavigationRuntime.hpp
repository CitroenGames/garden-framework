#pragma once

// Per-world bot navigation ownership.  The navmesh is a game asset rather
// than an entity, so it belongs in the game's registry context alongside the
// other authoritative per-world resources.  Keeping it here prevents a DLL
// static from leaking a previous level's mesh, timers, or bot input state into
// a newly created world.
#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "shared/BotRouteData.hpp"
#include "shared/BotLogic.hpp"

#include <cstdint>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace Bots {

struct NavigationDiagnosticRecord {
    uint32_t tick = 0;
    int32_t bot_index = 0;
    NavFailureCategory category = NavFailureCategory::None;
    LocalBlockDetail detail = LocalBlockDetail::None;
    uint32_t recovery_count = 0;
    bool capacity_inspection_suppressed = false;
};

// A world-owned ring gives operators the last navigation episodes without
// retaining pawn transforms, path points, or peer identifiers.
struct NavigationDiagnosticRing {
    static constexpr size_t kCapacity = 64;
    std::array<NavigationDiagnosticRecord, kCapacity> records{};
    size_t head = 0;
    size_t count = 0;
    uint64_t total_pushed = 0;

    void push(const NavigationDiagnosticRecord& record) {
        records[head] = record;
        head = (head + 1) % kCapacity;
        if (count < kCapacity) ++count;
        ++total_pushed;
    }
    size_t size() const { return count; }
    const NavigationDiagnosticRecord& oldest() const { return records[(head + kCapacity - count) % kCapacity]; }
    const NavigationDiagnosticRecord& newest() const { return records[(head + kCapacity - 1) % kCapacity]; }
    const NavigationDiagnosticRecord& at(size_t index) const { return records[(head + kCapacity - count + index) % kCapacity]; }
};

// Anonymous per-world records are retained only long enough for the staggered
// hearing pass.  They have no producer identity and never leave server memory.
struct NoiseRecord {
    glm::vec3 origin{0.0f};
    uint32_t emission_tick = 0;
    uint32_t serial = 0;
    float loudness = 0.0f;
    BotHeardNoiseCategory category = BotHeardNoiseCategory::None;
};

struct NoiseRing {
    static constexpr size_t kCapacity = 64;
    std::array<NoiseRecord, kCapacity> records{};
    size_t head = 0;
    size_t count = 0;
    uint32_t next_serial = 1;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t overwritten = 0;

    bool publish(BotHeardNoiseCategory category, const glm::vec3& origin,
                 uint32_t emission_tick, float loudness)
    {
        if (!isValidHearingCategory(category) || !isFiniteBotVector(origin) ||
            !std::isfinite(loudness) || loudness <= 0.0f || loudness > 1.0f ||
            next_serial == 0) {
            ++rejected;
            return false;
        }
        const uint32_t serial = next_serial++;
        records[head] = NoiseRecord{origin, emission_tick, serial, loudness, category};
        head = (head + 1) % kCapacity;
        if (count < kCapacity)
            ++count;
        else
            ++overwritten;
        ++accepted;
        return true;
    }

    size_t size() const { return count; }
    const NoiseRecord& at(size_t index) const
    {
        return records[(head + kCapacity - count + index) % kCapacity];
    }

    std::vector<BotHearingCandidate> hearingCandidates() const
    {
        std::vector<BotHearingCandidate> candidates;
        candidates.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const NoiseRecord& record = at(index);
            candidates.push_back(BotHearingCandidate{
                record.origin, record.emission_tick, record.serial,
                record.loudness, record.category});
        }
        return candidates;
    }
};

enum class NavMeshLoadStatus : uint8_t {
    NotRequested,
    Loaded,
    Missing,
    Invalid,
};

enum class RouteDataLoadStatus : uint8_t {
    NotRequested,
    Loaded,
    Missing,
    Invalid,
};

inline const char* routeDataLoadStatusName(RouteDataLoadStatus status)
{
    switch (status) {
    case RouteDataLoadStatus::NotRequested: return "not_requested";
    case RouteDataLoadStatus::Loaded: return "loaded";
    case RouteDataLoadStatus::Missing: return "missing";
    case RouteDataLoadStatus::Invalid: return "invalid";
    }
    return "unknown";
}

inline const char* navMeshLoadStatusName(NavMeshLoadStatus status)
{
    switch (status) {
    case NavMeshLoadStatus::NotRequested: return "not_requested";
    case NavMeshLoadStatus::Loaded: return "loaded";
    case NavMeshLoadStatus::Missing: return "missing";
    case NavMeshLoadStatus::Invalid: return "invalid";
    }
    return "unknown";
}

// A resource instance is owned by one registry/world.  It has no global
// cache: reset releases Detour allocations and clears every per-world bot
// counter or sequence before another level can start using this resource.
struct BotWorldRuntime {
    Navigation::NavMesh navmesh;
    std::string resolved_nav_path;
    uint32_t nav_format_version = 0;
    NavMeshLoadStatus nav_status = NavMeshLoadStatus::NotRequested;
    bool nav_attempted = false;
    AuthoredRouteData route_data;
    std::string resolved_route_path;
    std::string route_error;
    RouteDataLoadStatus route_status = RouteDataLoadStatus::NotRequested;
    bool route_attempted = false;
    // Handles are world-resource routing state only. The mutable tactical
    // facts stay on these two ECS component entities.
    std::array<entt::entity, kCompetitiveSiteCount> team_blackboard_entities{
        entt::null, entt::null};
    bool bots_enabled = false;
    bool error_logged = false;
    int32_t target_count = 0;
    uint32_t input_tick = 1;
    uint32_t path_schedule_tick = 1;
    float world_time = 0.0f;
    TransitionRingBuffer transitions;
    NavigationDiagnosticRing navigation_diagnostics;
    NoiseRing noise_ring;
    uint32_t capacity_diagnostic_calls = 0;
    uint32_t capacity_diagnostic_suppressions = 0;
    uint32_t navigation_recoveries = 0;
    uint32_t avoidance_queries = 0;
    uint32_t avoidance_samples = 0;
    uint32_t doorway_yields = 0;
    uint32_t doorway_repaths = 0;
    uint32_t capacity_diagnostic_next_tick = 0;
    float debug_last_dump = -10.0f;
    std::unordered_map<int32_t, StuckLogLatch> stuck_latches;
    std::unordered_map<uint16_t, uint32_t> buy_sequence;
    std::unordered_map<uint16_t, uint8_t> last_weapon_select;

    bool hasValidNavMesh() const { return bots_enabled && navmesh.valid; }

    bool publishNoise(BotHeardNoiseCategory category, const glm::vec3& origin,
                      uint32_t emission_tick, float loudness)
    {
        if (!hasValidNavMesh()) {
            ++noise_ring.rejected;
            return false;
        }
        return noise_ring.publish(category, origin, emission_tick, loudness);
    }

    bool beginCapacityDiagnostic(uint32_t tick)
    {
        // The schedule tick is a fixed server tick.  The 60-tick interval is
        // one second at the current 60 Hz server cadence and is independent
        // for every BotWorldRuntime.
        if (tick < capacity_diagnostic_next_tick) {
            ++capacity_diagnostic_suppressions;
            return false;
        }
        capacity_diagnostic_next_tick = tick + 60u;
        ++capacity_diagnostic_calls;
        return true;
    }

    void recordNavigationDiagnostic(uint32_t tick, int32_t bot_index,
                                    NavFailureCategory category, LocalBlockDetail detail,
                                    uint32_t recovery_count, bool capacity_suppressed = false)
    {
        if (category != NavFailureCategory::LocalBlock)
            detail = LocalBlockDetail::None;
        navigation_diagnostics.push(NavigationDiagnosticRecord{
            tick, bot_index, category, detail, recovery_count, capacity_suppressed});
    }

    // This aggregate is intentionally the only periodic navigation telemetry.
    // It gives operators a bounded per-world summary without publishing a
    // transform, route point, target, hit entity, or peer identifier.
    std::string formatNavigationDiagnosticSnapshot() const
    {
        const NavigationDiagnosticRecord empty{};
        const NavigationDiagnosticRecord& last = navigation_diagnostics.size() > 0
            ? navigation_diagnostics.newest() : empty;
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
            "[BotDebug] navigation_diagnostics count=%zu recoveries=%u capacity_calls=%u "
            "capacity_suppressions=%u avoidance_queries=%u avoidance_samples=%u "
            "doorway_yields=%u doorway_repaths=%u noise_accepted=%llu noise_rejected=%llu "
            "noise_overwritten=%llu last_tick=%u last_bot=%d last_category=%s "
            "last_detail=%s last_recovery=%u last_capacity_suppressed=%d",
            navigation_diagnostics.size(), navigation_recoveries,
            capacity_diagnostic_calls, capacity_diagnostic_suppressions,
            avoidance_queries, avoidance_samples, doorway_yields, doorway_repaths,
            static_cast<unsigned long long>(noise_ring.accepted),
            static_cast<unsigned long long>(noise_ring.rejected),
            static_cast<unsigned long long>(noise_ring.overwritten),
            last.tick, last.bot_index, navFailureCategoryName(last.category),
            localBlockDetailName(last.detail), last.recovery_count,
            last.capacity_inspection_suppressed ? 1 : 0);
        return std::string(buffer);
    }

    void reset()
    {
        navmesh.clear();
        resolved_nav_path.clear();
        nav_format_version = 0;
        nav_status = NavMeshLoadStatus::NotRequested;
        nav_attempted = false;
        route_data.clear();
        resolved_route_path.clear();
        route_error.clear();
        route_status = RouteDataLoadStatus::NotRequested;
        route_attempted = false;
        team_blackboard_entities.fill(entt::null);
        bots_enabled = false;
        error_logged = false;
        target_count = 0;
        input_tick = 1;
        path_schedule_tick = 1;
        world_time = 0.0f;
        transitions = {};
        navigation_diagnostics = {};
        noise_ring = {};
        capacity_diagnostic_calls = 0;
        capacity_diagnostic_suppressions = 0;
        navigation_recoveries = 0;
        avoidance_queries = 0;
        avoidance_samples = 0;
        doorway_yields = 0;
        doorway_repaths = 0;
        capacity_diagnostic_next_tick = 0;
        debug_last_dump = -10.0f;
        stuck_latches.clear();
        buy_sequence.clear();
        last_weapon_select.clear();
    }

    // The caller supplies an already-resolved project asset path.  Loading a
    // missing or malformed artifact always clears an earlier good mesh, so a
    // level transition cannot run bots against stale geometry.
    bool loadResolvedNavMesh(const std::string& path)
    {
        navmesh.clear();
        resolved_nav_path = path;
        nav_attempted = true;
        bots_enabled = false;
        if (path.empty()) {
            nav_status = NavMeshLoadStatus::Missing;
            return false;
        }

        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            nav_status = NavMeshLoadStatus::Missing;
            return false;
        }

        Navigation::NavMesh candidate;
        if (!Navigation::NavMeshSerializer::load(candidate, path)) {
            nav_status = NavMeshLoadStatus::Invalid;
            return false;
        }
        if (!candidate.valid || candidate.total_polys <= 0) {
            candidate.clear();
            nav_status = NavMeshLoadStatus::Invalid;
            return false;
        }

        navmesh = std::move(candidate);
        nav_format_version = Navigation::NavMeshSerializer::kCurrentVersion;
        nav_status = NavMeshLoadStatus::Loaded;
        bots_enabled = true;
        return true;
    }

    // Tactical data is optional.  A missing sidecar deliberately leaves bots
    // on the canonical bomb-site fallback; malformed authored data fails
    // closed and is never retained from a previous level.
    bool loadResolvedRouteData(const std::string& path)
    {
        route_data.clear();
        resolved_route_path = path;
        route_error.clear();
        route_attempted = true;
        if (path.empty()) {
            route_status = RouteDataLoadStatus::Missing;
            route_error = "asset path is empty";
            return false;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            route_status = RouteDataLoadStatus::Missing;
            route_error = "asset file is missing";
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::string error;
        if (!parseAuthoredRouteData(text, route_data, &error)) {
            route_data.clear();
            route_status = RouteDataLoadStatus::Invalid;
            route_error = error;
            return false;
        }
        route_status = RouteDataLoadStatus::Loaded;
        return true;
    }
};

} // namespace Bots
