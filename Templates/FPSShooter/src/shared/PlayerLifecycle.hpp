#pragma once

#include "CompetitiveLogic.hpp"
#include "MovementInputPolicy.hpp"

#include <cstdint>
#include <entt/entt.hpp>

namespace FPSShooter
{
    // Template-owned presentation and control lifecycle. Health, team, and
    // round phase remain authoritative in their existing components; this
    // component records their player-facing consequence for the pawn.
    enum class FpsPawnLifecycleState : uint8_t
    {
        Active = 0,
        AwaitingSpawn,
        Spectating,
    };

    struct FpsPawnLifecycleComponent
    {
        FpsPawnLifecycleState state = FpsPawnLifecycleState::Active;
    };

    inline FpsPawnLifecycleState derivePawnLifecycleState(
        const FpsHealthComponent& health,
        const TeamComponent& team,
        Competitive::RoundPhase phase)
    {
        if (!Competitive::isPlayingTeam(Competitive::teamFromId(team.team_id)))
            return FpsPawnLifecycleState::Spectating;
        if (health.alive)
            return FpsPawnLifecycleState::Active;
        return Competitive::canSpawnNow(phase)
            ? FpsPawnLifecycleState::AwaitingSpawn
            : FpsPawnLifecycleState::Spectating;
    }

    inline bool isPawnMovementLocked(Competitive::RoundPhase phase)
    {
        return phase == Competitive::RoundPhase::Freeze;
    }

    inline bool isPawnMovementAllowed(
        const FpsHealthComponent& health,
        const TeamComponent& team,
        bool competitive,
        Competitive::RoundPhase phase)
    {
        if (competitive && !Competitive::isPlayingTeam(Competitive::teamFromId(team.team_id)))
            return false;
        return isMovementInputAllowed(
            health.alive,
            competitive && isPawnMovementLocked(phase));
    }

    inline void synchronizePawnLifecycles(
        entt::registry& registry,
        const Competitive::MatchStateComponent& match_state)
    {
        auto pawns = registry.view<FpsHealthComponent, TeamComponent>();
        for (const entt::entity pawn : pawns)
        {
            auto& lifecycle = registry.get_or_emplace<FpsPawnLifecycleComponent>(pawn);
            lifecycle.state = derivePawnLifecycleState(
                pawns.get<FpsHealthComponent>(pawn),
                pawns.get<TeamComponent>(pawn),
                match_state.phase);
        }
    }
}
