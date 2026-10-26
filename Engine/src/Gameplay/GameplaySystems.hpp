#pragma once

#include "EngineExport.h"
#include "Gameplay/GameplayComponents.hpp"
#include "Scheduler/SystemScheduler.hpp"
#include "Tick/TickSystem.hpp"

#include <cstdint>
#include <vector>
#include <entt/entt.hpp>

// Single server-authoritative entry point for damage.
class ENGINE_API DamageSystem
{
public:
    static DamageResult applyDamage(entt::registry& registry, entt::entity target,
                                    const DamageInfo& info, uint64_t current_tick);
    // Convenience: tick 0 (invulnerability only blocks when until_tick > 0).
    static DamageResult applyDamage(entt::registry& registry, entt::entity target,
                                    const DamageInfo& info);
};

namespace Gameplay
{
// Roster query helpers (TeamComponent).
ENGINE_API size_t countTeamMembers(const entt::registry& registry, uint8_t team_id);
ENGINE_API std::vector<entt::entity> getTeamMembers(const entt::registry& registry, uint8_t team_id);
inline std::vector<entt::entity> teamRoster(const entt::registry& r, uint8_t t)
{
    return getTeamMembers(r, t);
}
inline std::vector<entt::entity> collectTeamMembers(const entt::registry& r, uint8_t t)
{
    return getTeamMembers(r, t);
}
inline size_t teamMemberCount(const entt::registry& r, uint8_t t)
{
    return countTeamMembers(r, t);
}
ENGINE_API bool isOnTeam(const entt::registry& registry, entt::entity e, uint8_t team_id);
ENGINE_API bool areTeammates(const entt::registry& registry, entt::entity a, entt::entity b);

// Channel helpers with event emission.
ENGINE_API bool startChannel(entt::registry& registry, entt::entity e, uint32_t channel_id, float duration);
ENGINE_API bool cancelChannel(entt::registry& registry, entt::entity e, uint64_t current_tick = 0);
} // namespace Gameplay

// Free-function roster aliases (discoverability).
inline size_t countTeamMembers(const entt::registry& r, uint8_t t)
{
    return Gameplay::countTeamMembers(r, t);
}
inline std::vector<entt::entity> getTeamMembers(const entt::registry& r, uint8_t t)
{
    return Gameplay::getTeamMembers(r, t);
}

// Fixed-tick systems (Gameplay phase when registered).
ENGINE_API void tickCountdownTimers(entt::registry& registry, const Tick::SimulationTickContext& ctx);
ENGINE_API void tickChannels(entt::registry& registry, const Tick::SimulationTickContext& ctx);

// Registers "engine.gameplay.countdown_timers" and
// "engine.gameplay.channels" in the Gameplay phase.
ENGINE_API void registerGameplaySystems(SystemScheduler& scheduler);
ENGINE_API bool registerGameplayPrimitives(SystemScheduler& scheduler);
