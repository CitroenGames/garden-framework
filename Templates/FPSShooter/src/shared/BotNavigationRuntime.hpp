#pragma once

// Per-world bot navigation ownership.  The navmesh is a game asset rather
// than an entity, so it belongs in the game's registry context alongside the
// other authoritative per-world resources.  Keeping it here prevents a DLL
// static from leaking a previous level's mesh, timers, or bot input state into
// a newly created world.
#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "shared/BotLogic.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace Bots {

enum class NavMeshLoadStatus : uint8_t {
    NotRequested,
    Loaded,
    Missing,
    Invalid,
};

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
    bool bots_enabled = false;
    bool error_logged = false;
    int32_t target_count = 0;
    uint32_t input_tick = 1;
    float world_time = 0.0f;
    TransitionRingBuffer transitions;
    float debug_last_dump = -10.0f;
    std::unordered_map<int32_t, StuckLogLatch> stuck_latches;
    std::unordered_map<uint16_t, uint32_t> buy_sequence;
    std::unordered_map<uint16_t, uint8_t> last_weapon_select;

    bool hasValidNavMesh() const { return bots_enabled && navmesh.valid; }

    void reset()
    {
        navmesh.clear();
        resolved_nav_path.clear();
        nav_format_version = 0;
        nav_status = NavMeshLoadStatus::NotRequested;
        nav_attempted = false;
        bots_enabled = false;
        error_logged = false;
        target_count = 0;
        input_tick = 1;
        world_time = 0.0f;
        transitions = {};
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
};

} // namespace Bots
