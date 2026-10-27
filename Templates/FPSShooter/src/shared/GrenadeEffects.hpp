#pragma once

#include "Components/Components.hpp"
#include "SharedComponents.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>

// Template-owned grenade authority.  The server is the only caller of these
// registry functions.  This file intentionally contains no effect values,
// damage rules, collision queries, or presentation events: the current weapon
// catalog has no approved grenade gameplay definitions.  A future authoritative
// effect rule may consume the sorted target list and route approved damage only
// through FpsServer::applyFpsDamage.
namespace FpsLogic {

enum class FpsGrenadeEffectKind : uint8_t {
    None = 0,
    HighExplosive,
    Flash,
    Smoke,
    Fire,
    Decoy,
};

// The projectile keeps both its entity owner and value ownership.  The latter
// remains valid after a disconnect, while a future damage rule can safely let
// C1 reject a stale owner entity.
struct FpsGrenadeProjectileComponent {
    WeaponType weapon_type = WeaponType::GRENADE;
    FpsGrenadeEffectKind effect_kind = FpsGrenadeEffectKind::None;
    entt::entity owner = entt::null;
    uint16_t owner_client_id = 0;
    uint8_t owner_team_id = 0;
    glm::vec3 velocity{0.0f};
    // Collision is resolved by the server's world-query adapter.  Store the
    // reported point as value data so the fixed-tick detonation is independent
    // of callback timing and cannot read a stale collision object.
    glm::vec3 impact_position{0.0f};
    float fuse_remaining = 0.0f;
    float effect_duration = 0.0f;
    bool impact_requested = false;
    bool detonation_latched = false;
};

// Effect ownership is value data for deterministic cleanup and future
// attribution.  Current effects have lifetime only; no gameplay mutation is
// inferred from their kind or from the weapon catalog.
struct FpsGrenadeEffectComponent {
    FpsGrenadeEffectKind kind = FpsGrenadeEffectKind::None;
    entt::entity owner = entt::null;
    uint16_t owner_client_id = 0;
    uint8_t owner_team_id = 0;
    float remaining = 0.0f;
};

struct FpsGrenadeProjectileSpawn {
    WeaponType weapon_type = WeaponType::GRENADE;
    FpsGrenadeEffectKind effect_kind = FpsGrenadeEffectKind::None;
    entt::entity owner = entt::null;
    uint16_t owner_client_id = 0;
    uint8_t owner_team_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float fuse_seconds = 0.0f;
    float effect_duration_seconds = 0.0f;
};

struct FpsGrenadeTickResult {
    std::vector<entt::entity> detonated_projectiles;
    std::vector<entt::entity> created_effects;
};

struct FpsGrenadeEffectTarget {
    entt::entity entity = entt::null;
    uint32_t stable_order = 0;
};

inline bool isGrenadeWeaponType(WeaponType type)
{
    switch (type) {
    case WeaponType::GRENADE:
    case WeaponType::HEGRENADE:
    case WeaponType::FLASHBANG:
    case WeaponType::SMOKEGRENADE:
    case WeaponType::MOLOTOV:
    case WeaponType::INCENDIARY:
    case WeaponType::DECOY:
        return true;
    default:
        return false;
    }
}

inline bool isFiniteGrenadeVector(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool isValidGrenadeProjectileSpawn(const FpsGrenadeProjectileSpawn& spawn)
{
    return isGrenadeWeaponType(spawn.weapon_type) &&
        spawn.effect_kind != FpsGrenadeEffectKind::None &&
        isFiniteGrenadeVector(spawn.position) && isFiniteGrenadeVector(spawn.velocity) &&
        std::isfinite(spawn.fuse_seconds) && spawn.fuse_seconds >= 0.0f &&
        std::isfinite(spawn.effect_duration_seconds) && spawn.effect_duration_seconds >= 0.0f;
}

inline entt::entity spawnGrenadeProjectile(
    entt::registry& registry,
    const FpsGrenadeProjectileSpawn& spawn)
{
    if (!isValidGrenadeProjectileSpawn(spawn))
        return entt::null;

    const entt::entity projectile = registry.create();
    registry.emplace<TransformComponent>(projectile, spawn.position.x, spawn.position.y, spawn.position.z);
    registry.emplace<FpsGrenadeProjectileComponent>(projectile, FpsGrenadeProjectileComponent{
        spawn.weapon_type, spawn.effect_kind, spawn.owner, spawn.owner_client_id, spawn.owner_team_id,
        spawn.velocity, spawn.position, spawn.fuse_seconds, spawn.effect_duration_seconds, false, false});
    return projectile;
}

// Collision integration is intentionally outside this foundation.  The server
// collision path may request an impact; the next fixed tick owns detonation.
inline bool requestGrenadeProjectileImpact(
    entt::registry& registry,
    entt::entity projectile,
    const glm::vec3& impact_position)
{
    auto* state = registry.try_get<FpsGrenadeProjectileComponent>(projectile);
    if (!state || state->detonation_latched || !isFiniteGrenadeVector(impact_position))
        return false;
    state->impact_position = impact_position;
    state->impact_requested = true;
    return true;
}

// Legacy callers that only know the projectile entity preserve the last
// authoritative simulation position.  New collision adapters must use the
// point-taking overload, which records the actual fixed-tick detonation point.
inline bool requestGrenadeProjectileImpact(entt::registry& registry, entt::entity projectile)
{
    const auto* transform = registry.try_get<TransformComponent>(projectile);
    return transform != nullptr && requestGrenadeProjectileImpact(
        registry, projectile, transform->position);
}

// Produce a stable entity order before a future area/LOS rule examines targets.
// Entity creation order is the current authority identity; no client ordering or
// unordered container traversal can affect the resulting sequence.
inline std::vector<FpsGrenadeEffectTarget> collectLiveGrenadeEffectTargets(
    const entt::registry& registry)
{
    std::vector<FpsGrenadeEffectTarget> targets;
    const auto view = registry.view<FpsHealthComponent, TeamComponent, TransformComponent>();
    for (const entt::entity entity : view) {
        if (!view.get<FpsHealthComponent>(entity).alive)
            continue;
        targets.push_back({entity, static_cast<uint32_t>(entity)});
    }
    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        return left.stable_order < right.stable_order;
    });
    return targets;
}

inline void clearGrenadeEffects(entt::registry& registry)
{
    std::vector<entt::entity> entities;
    const auto projectiles = registry.view<FpsGrenadeProjectileComponent>();
    const auto effects = registry.view<FpsGrenadeEffectComponent>();
    for (const entt::entity entity : projectiles)
        entities.push_back(entity);
    for (const entt::entity entity : effects)
        if (!registry.all_of<FpsGrenadeProjectileComponent>(entity))
            entities.push_back(entity);
    std::sort(entities.begin(), entities.end(), [](const entt::entity left, const entt::entity right) {
        return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
    });
    for (const entt::entity entity : entities)
        if (registry.valid(entity))
            registry.destroy(entity);
}

// Advance one server fixed tick.  A false active_round performs the mandatory
// round cleanup before any fuse or effect work, so effects cannot spill into a
// subsequent round.  Detonated projectiles are destroyed in the same call,
// providing a second guard against duplicate detonation.
inline FpsGrenadeTickResult tickGrenadeEffects(
    entt::registry& registry,
    float fixed_delta,
    bool active_round)
{
    FpsGrenadeTickResult result;
    if (!active_round) {
        clearGrenadeEffects(registry);
        return result;
    }
    if (!std::isfinite(fixed_delta) || fixed_delta <= 0.0f)
        return result;

    std::vector<entt::entity> expired_effects;
    const auto effects = registry.view<FpsGrenadeEffectComponent>();
    for (const entt::entity effect : effects) {
        auto& state = effects.get<FpsGrenadeEffectComponent>(effect);
        state.remaining -= fixed_delta;
        if (!std::isfinite(state.remaining) || state.remaining <= 0.0f)
            expired_effects.push_back(effect);
    }
    std::sort(expired_effects.begin(), expired_effects.end(), [](const entt::entity left, const entt::entity right) {
        return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
    });
    for (const entt::entity effect : expired_effects)
        if (registry.valid(effect))
            registry.destroy(effect);

    std::vector<entt::entity> projectiles;
    const auto projectile_view = registry.view<FpsGrenadeProjectileComponent, TransformComponent>();
    for (const entt::entity projectile : projectile_view)
        projectiles.push_back(projectile);
    std::sort(projectiles.begin(), projectiles.end(), [](const entt::entity left, const entt::entity right) {
        return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
    });

    for (const entt::entity projectile : projectiles) {
        if (!registry.valid(projectile))
            continue;
        auto& state = registry.get<FpsGrenadeProjectileComponent>(projectile);
        auto& transform = registry.get<TransformComponent>(projectile);
        if (state.detonation_latched || state.effect_kind == FpsGrenadeEffectKind::None ||
            !isFiniteGrenadeVector(transform.position) || !std::isfinite(state.fuse_remaining) ||
            !isFiniteGrenadeVector(state.velocity) || !isFiniteGrenadeVector(state.impact_position) ||
            !std::isfinite(state.effect_duration) || state.fuse_remaining < 0.0f ||
            state.effect_duration < 0.0f) {
            registry.destroy(projectile);
            continue;
        }

        const bool detonate = state.impact_requested || state.fuse_remaining <= fixed_delta;
        if (!detonate) {
            // Projectile motion is simulation state.  Collision adapters run
            // before this function and may replace the position through the
            // impact request path; presentation never participates here.
            transform.position += state.velocity * fixed_delta;
            state.fuse_remaining -= fixed_delta;
            continue;
        }

        state.detonation_latched = true;
        if (state.impact_requested)
            transform.position = state.impact_position;
        const entt::entity effect = registry.create();
        registry.emplace<TransformComponent>(effect, transform.position.x, transform.position.y, transform.position.z);
        registry.emplace<FpsGrenadeEffectComponent>(effect, FpsGrenadeEffectComponent{
            state.effect_kind, state.owner, state.owner_client_id, state.owner_team_id, state.effect_duration});
        result.detonated_projectiles.push_back(projectile);
        result.created_effects.push_back(effect);
        registry.destroy(projectile);
    }
    return result;
}

} // namespace FpsLogic
