#pragma once

#include "CompetitiveLogic.hpp"
#include "MovementInputPolicy.hpp"

#include "Components/IdentityComponent.hpp"

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

    // Template-owned durable observer state. The EntityRef keeps a chase
    // selection safe if an entity slot is destroyed and later reused. Camera
    // presentation and network message routing intentionally remain outside
    // this game-state contract.
    enum class FpsSpectatorMode : uint8_t
    {
        Free = 0,
        Chase,
    };

    struct FpsSpectatorComponent
    {
        FpsSpectatorMode mode = FpsSpectatorMode::Free;
        EntityRef target{};
        bool eligible = false;
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

    inline bool isSpectatorEligible(
        const FpsHealthComponent& health,
        const TeamComponent& team,
        Competitive::RoundPhase phase)
    {
        const Competitive::FpsTeam source_team = Competitive::teamFromId(team.team_id);
        if (source_team == Competitive::FpsTeam::Spectator)
            return true;
        return Competitive::isPlayingTeam(source_team) && !health.alive &&
            !Competitive::canSpawnNow(phase);
    }

    inline void resetSpectatorState(FpsSpectatorComponent& spectator)
    {
        spectator.mode = FpsSpectatorMode::Free;
        spectator.target = EntityRef{};
    }

    // Restore the template-owned pawn state at a confirmed respawn boundary.
    // Health, the once-only death latch, loadout/inventory mirror, and local
    // observer presentation must move together: otherwise a pawn can be alive
    // with a stale dead/spectating presentation until a later world tick.
    // The caller remains responsible for world position, physics body, and
    // replication because those belong to the server spawn operation.
    inline bool restorePawnAfterRespawn(
        entt::registry& registry,
        entt::entity pawn,
        WeaponType primary_weapon)
    {
        if (pawn == entt::null || !registry.valid(pawn) ||
            !registry.all_of<FpsHealthComponent, InventoryComponent,
                             PlayerLoadoutComponent, WeaponComponent>(pawn))
            return false;

        auto& health = registry.get<FpsHealthComponent>(pawn);
        auto& inventory = registry.get<InventoryComponent>(pawn);
        auto& loadout = registry.get<PlayerLoadoutComponent>(pawn);
        auto& weapon = registry.get<WeaponComponent>(pawn);

        health.reset();
        // Preserve the monotonic sequence for attribution while reopening the
        // next death transition.  This duplicates no server policy; it is the
        // ECS state reset that both local and networked respawns require.
        registry.get_or_emplace<FpsDeathLifecycleComponent>(pawn).resolved = false;
        FpsLogic::grantLoadout(inventory, loadout, weapon, primary_weapon);

        auto& lifecycle = registry.get_or_emplace<FpsPawnLifecycleComponent>(pawn);
        lifecycle.state = FpsPawnLifecycleState::Active;
        auto& spectator = registry.get_or_emplace<FpsSpectatorComponent>(pawn);
        spectator.eligible = false;
        resetSpectatorState(spectator);
        return FpsLogic::hasMirroredLoadoutInventory(inventory, loadout);
    }

    inline bool setSpectatorFree(entt::registry& registry, entt::entity spectator_entity)
    {
        if (!registry.valid(spectator_entity) ||
            !registry.all_of<FpsHealthComponent, TeamComponent>(spectator_entity))
            return false;
        resetSpectatorState(registry.get_or_emplace<FpsSpectatorComponent>(spectator_entity));
        return true;
    }

    inline bool isValidSpectatorTarget(
        entt::registry& registry,
        entt::entity spectator_entity,
        entt::entity target_entity,
        Competitive::RoundPhase phase)
    {
        if (!registry.valid(spectator_entity) || !registry.valid(target_entity) ||
            spectator_entity == target_entity || Competitive::canSpawnNow(phase))
            return false;

        const auto* spectator_health = registry.try_get<FpsHealthComponent>(spectator_entity);
        const auto* spectator_team = registry.try_get<TeamComponent>(spectator_entity);
        const auto* target_health = registry.try_get<FpsHealthComponent>(target_entity);
        const auto* target_team = registry.try_get<TeamComponent>(target_entity);
        if (spectator_health == nullptr || spectator_team == nullptr ||
            target_health == nullptr || target_team == nullptr ||
            !isSpectatorEligible(*spectator_health, *spectator_team, phase) ||
            !target_health->alive || target_health->health <= 0)
            return false;

        const Competitive::FpsTeam source_team = Competitive::teamFromId(spectator_team->team_id);
        return Competitive::isPlayingTeam(source_team) &&
            target_team->team_id == spectator_team->team_id;
    }

    inline bool setSpectatorChaseTarget(
        entt::registry& registry,
        entt::entity spectator_entity,
        entt::entity target_entity,
        Competitive::RoundPhase phase)
    {
        if (!isValidSpectatorTarget(registry, spectator_entity, target_entity, phase))
            return false;

        auto& spectator = registry.get_or_emplace<FpsSpectatorComponent>(spectator_entity);
        spectator.eligible = true;
        spectator.mode = FpsSpectatorMode::Chase;
        spectator.target = ensureEntityRef(registry, target_entity);
        return true;
    }

    inline void synchronizeSpectatorStates(
        entt::registry& registry,
        const Competitive::MatchStateComponent& match_state)
    {
        auto pawns = registry.view<FpsHealthComponent, TeamComponent>();
        for (const entt::entity pawn : pawns)
        {
            const auto& health = pawns.get<FpsHealthComponent>(pawn);
            const auto& team = pawns.get<TeamComponent>(pawn);
            auto& spectator = registry.get_or_emplace<FpsSpectatorComponent>(pawn);
            spectator.eligible = isSpectatorEligible(health, team, match_state.phase);
            if (!spectator.eligible)
            {
                resetSpectatorState(spectator);
                continue;
            }

            if (spectator.mode != FpsSpectatorMode::Chase)
            {
                resetSpectatorState(spectator);
                continue;
            }

            const entt::entity target = resolveEntityRef(registry, spectator.target);
            if (!isValidSpectatorTarget(registry, pawn, target, match_state.phase))
                resetSpectatorState(spectator);
        }
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
