#pragma once

#include "Gameplay/GameplayComponents.hpp"
#include "shared/CompetitiveLogic.hpp"
#include "shared/SharedComponents.hpp"

#include <cstdint>
#include <entt/entt.hpp>

// Server-owned FPSShooter damage contract. Callers supply already-authoritative
// trace/objective results; this boundary owns only validation and health mutation.
namespace FpsServer {

enum class DamageRejectReason : uint8_t {
    None = 0,
    InvalidEntity,
    SelfTarget,
    InvalidDamage,
    MissingHealth,
    AttackerDead,
    VictimDead,
    Invulnerable,
    PhaseBlocked,
    InvalidTeam,
    SameTeam,
};

struct DamageRequest {
    entt::entity attacker = entt::null;
    entt::entity victim = entt::null;
    int32_t damage = 0;
};

struct DamagePolicy {
    bool require_live_phase = false;
    Competitive::RoundPhase phase = Competitive::RoundPhase::Live;
    bool require_opposing_teams = false;
};

struct DamageResult {
    DamageRejectReason reject_reason = DamageRejectReason::None;
    int32_t health_damage = 0;
    bool applied = false;
    bool killed = false;
};

inline DamageResult applyFpsDamage(
    entt::registry& registry,
    const DamageRequest& request,
    const DamagePolicy& policy)
{
    DamageResult result;
    if (request.attacker == entt::null || request.victim == entt::null ||
        !registry.valid(request.attacker) || !registry.valid(request.victim)) {
        result.reject_reason = DamageRejectReason::InvalidEntity;
        return result;
    }
    if (request.attacker == request.victim) {
        result.reject_reason = DamageRejectReason::SelfTarget;
        return result;
    }
    if (request.damage <= 0) {
        result.reject_reason = DamageRejectReason::InvalidDamage;
        return result;
    }
    if (!registry.all_of<FpsHealthComponent>(request.attacker) ||
        !registry.all_of<FpsHealthComponent>(request.victim)) {
        result.reject_reason = DamageRejectReason::MissingHealth;
        return result;
    }

    const auto& attacker_health = registry.get<FpsHealthComponent>(request.attacker);
    auto& victim_health = registry.get<FpsHealthComponent>(request.victim);
    if (!attacker_health.alive) {
        result.reject_reason = DamageRejectReason::AttackerDead;
        return result;
    }
    if (!victim_health.alive) {
        result.reject_reason = DamageRejectReason::VictimDead;
        return result;
    }
    if (victim_health.invulnerable_timer > 0.0f) {
        result.reject_reason = DamageRejectReason::Invulnerable;
        return result;
    }
    if (policy.require_live_phase && policy.phase != Competitive::RoundPhase::Live) {
        result.reject_reason = DamageRejectReason::PhaseBlocked;
        return result;
    }
    if (policy.require_opposing_teams) {
        if (!registry.all_of<TeamComponent>(request.attacker) ||
            !registry.all_of<TeamComponent>(request.victim)) {
            result.reject_reason = DamageRejectReason::InvalidTeam;
            return result;
        }
        const Competitive::FpsTeam attacker_team =
            Competitive::teamFromId(registry.get<TeamComponent>(request.attacker).team_id);
        const Competitive::FpsTeam victim_team =
            Competitive::teamFromId(registry.get<TeamComponent>(request.victim).team_id);
        if (!Competitive::isPlayingTeam(attacker_team) || !Competitive::isPlayingTeam(victim_team)) {
            result.reject_reason = DamageRejectReason::InvalidTeam;
            return result;
        }
        if (attacker_team == victim_team) {
            result.reject_reason = DamageRejectReason::SameTeam;
            return result;
        }
    }

    const int32_t health_before = victim_health.health;
    victim_health.takeDamage(request.damage);
    result.health_damage = health_before - victim_health.health;
    result.applied = result.health_damage > 0;
    result.killed = result.applied && !victim_health.alive;
    return result;
}

} // namespace FpsServer
