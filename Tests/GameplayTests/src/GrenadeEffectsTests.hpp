// C4 grenade-effect foundation tests included by GameplayTests main.cpp.
#include "shared/GrenadeEffects.hpp"

#include <cstddef>
#include <limits>
#include <string>

static std::size_t grenadeEffectCount(entt::registry& registry)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto entity : registry.view<FpsLogic::FpsGrenadeEffectComponent>())
        ++count;
    return count;
}

bool testGrenadeEffectLifecycle()
{
    const std::string name = "Grenade_EffectLifecycle";
    entt::registry registry;
    const entt::entity owner = registry.create();
    const entt::entity early = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::SMOKEGRENADE, FpsLogic::FpsGrenadeEffectKind::Smoke, owner, 17, FpsLogic::kTeamAttackers,
        glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.10f, 0.20f});
    const entt::entity late = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::DECOY, FpsLogic::FpsGrenadeEffectKind::Decoy, owner, 17, FpsLogic::kTeamAttackers,
        glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.0f), 0.20f, 0.20f});
    if (early == entt::null || late == entt::null)
        return fail(name, "valid grenade projectiles were rejected");

    const auto first = FpsLogic::tickGrenadeEffects(registry, 0.10f, true);
    if (first.detonated_projectiles.size() != 1 || first.detonated_projectiles.front() != early ||
        first.created_effects.size() != 1 || registry.valid(early) || !registry.valid(late))
        return fail(name, "fuse boundary did not detonate exactly the first projectile");
    const auto& smoke = registry.get<FpsLogic::FpsGrenadeEffectComponent>(first.created_effects.front());
    if (smoke.kind != FpsLogic::FpsGrenadeEffectKind::Smoke || smoke.owner != owner ||
        smoke.owner_client_id != 17 || smoke.owner_team_id != FpsLogic::kTeamAttackers)
        return fail(name, "effect lost its owner or team value data");

    const auto second = FpsLogic::tickGrenadeEffects(registry, 0.10f, true);
    if (second.detonated_projectiles.size() != 1 || second.detonated_projectiles.front() != late ||
        second.created_effects.size() != 1 || registry.valid(late))
        return fail(name, "second fuse boundary did not produce one effect");
    const auto duplicate = FpsLogic::tickGrenadeEffects(registry, 0.01f, true);
    if (!duplicate.detonated_projectiles.empty() || !duplicate.created_effects.empty())
        return fail(name, "destroyed projectile detonated twice");

    FpsLogic::tickGrenadeEffects(registry, 0.20f, true);
    if (!registry.view<FpsLogic::FpsGrenadeEffectComponent>().empty())
        return fail(name, "expired grenade effect was not cleaned up");
    return pass(name);
}

bool testGrenadeEffectImpactTargetsAndCleanup()
{
    const std::string name = "Grenade_ImpactTargetsCleanup";
    entt::registry registry;
    const entt::entity owner = registry.create();
    const entt::entity disconnected_owner = registry.create();
    const entt::entity first_target = registry.create();
    const entt::entity dead_target = registry.create();
    const entt::entity second_target = registry.create();
    for (const entt::entity target : {first_target, dead_target, second_target}) {
        registry.emplace<FpsHealthComponent>(target, 100);
        registry.emplace<TeamComponent>(target, TeamComponent{FpsLogic::kTeamDefenders});
        registry.emplace<TransformComponent>(target);
    }
    registry.get<FpsHealthComponent>(dead_target).alive = false;
    const auto targets = FpsLogic::collectLiveGrenadeEffectTargets(registry);
    if (targets.size() != 2 || targets[0].stable_order >= targets[1].stable_order ||
        targets[0].entity != first_target || targets[1].entity != second_target)
        return fail(name, "live effect targets were not entity-stable and filtered");

    const entt::entity impact = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::FLASHBANG, FpsLogic::FpsGrenadeEffectKind::Flash, disconnected_owner, 23, FpsLogic::kTeamDefenders,
        glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(0.0f), 5.0f, 1.0f});
    if (!FpsLogic::requestGrenadeProjectileImpact(registry, impact) ||
        FpsLogic::requestGrenadeProjectileImpact(registry, entt::null))
        return fail(name, "impact request validation was wrong");
    registry.destroy(disconnected_owner);
    const auto impact_tick = FpsLogic::tickGrenadeEffects(registry, 0.01f, true);
    if (impact_tick.detonated_projectiles.size() != 1 || impact_tick.detonated_projectiles.front() != impact ||
        impact_tick.created_effects.size() != 1)
        return fail(name, "server impact did not detonate once before the fuse");
    const auto& flash = registry.get<FpsLogic::FpsGrenadeEffectComponent>(impact_tick.created_effects.front());
    if (flash.owner != disconnected_owner || flash.owner_client_id != 23 ||
        flash.owner_team_id != FpsLogic::kTeamDefenders)
        return fail(name, "disconnect-safe effect ownership changed");

    const entt::entity round_cleanup = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::SMOKEGRENADE, FpsLogic::FpsGrenadeEffectKind::Smoke, owner, 17, FpsLogic::kTeamAttackers,
        glm::vec3(0.0f), glm::vec3(0.0f), 5.0f, 1.0f});
    if (round_cleanup == entt::null)
        return fail(name, "round cleanup fixture did not spawn");
    FpsLogic::tickGrenadeEffects(registry, 0.01f, false);
    if (!registry.view<FpsLogic::FpsGrenadeProjectileComponent>().empty() ||
        !registry.view<FpsLogic::FpsGrenadeEffectComponent>().empty())
        return fail(name, "round/world cleanup left grenade authority entities behind");
    return pass(name);
}

bool testGrenadeProjectileMotionAndAuthoritativeImpact()
{
    const std::string name = "Grenade_MotionImpactResolution";
    entt::registry registry;
    const entt::entity projectile = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::HEGRENADE, FpsLogic::FpsGrenadeEffectKind::HighExplosive, entt::null, 41,
        FpsLogic::kTeamAttackers, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(8.0f, -4.0f, 2.0f),
        1.0f, 0.5f});
    if (projectile == entt::null)
        return fail(name, "valid moving projectile was rejected");

    const auto moving_tick = FpsLogic::tickGrenadeEffects(registry, 0.25f, true);
    if (!moving_tick.detonated_projectiles.empty() ||
        !approxVec3(registry.get<TransformComponent>(projectile).position, glm::vec3(3.0f, 1.0f, 3.5f)))
        return fail(name, "fixed tick did not advance projectile simulation position");

    if (FpsLogic::requestGrenadeProjectileImpact(
            registry, projectile, glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f)) ||
        !FpsLogic::requestGrenadeProjectileImpact(registry, projectile, glm::vec3(9.0f, 4.0f, -2.0f)))
        return fail(name, "authoritative impact point validation was wrong");

    const auto impact_tick = FpsLogic::tickGrenadeEffects(registry, 0.01f, true);
    if (impact_tick.detonated_projectiles.size() != 1 ||
        impact_tick.detonated_projectiles.front() != projectile || impact_tick.created_effects.size() != 1 ||
        registry.valid(projectile))
        return fail(name, "impact did not consume the projectile exactly once");
    const auto effect = impact_tick.created_effects.front();
    if (!approxVec3(registry.get<TransformComponent>(effect).position, glm::vec3(9.0f, 4.0f, -2.0f)) ||
        FpsLogic::requestGrenadeProjectileImpact(registry, projectile, glm::vec3(0.0f)))
        return fail(name, "effect did not resolve at the server collision point");
    return pass(name);
}

bool testGrenadeOverlapsSurviveOwnerDeath()
{
    const std::string name = "Grenade_OverlapOwnerDeath";
    entt::registry registry;
    const entt::entity owner = registry.create();
    registry.emplace<FpsHealthComponent>(owner, 100);
    const entt::entity first = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::SMOKEGRENADE, FpsLogic::FpsGrenadeEffectKind::Smoke, owner, 51,
        FpsLogic::kTeamAttackers, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f), 5.0f, 0.20f});
    const entt::entity second = FpsLogic::spawnGrenadeProjectile(registry, {
        WeaponType::FLASHBANG, FpsLogic::FpsGrenadeEffectKind::Flash, owner, 51,
        FpsLogic::kTeamAttackers, glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f), 5.0f, 0.30f});
    registry.get<FpsHealthComponent>(owner).takeDamage(100);
    if (!FpsLogic::requestGrenadeProjectileImpact(registry, first, glm::vec3(5.0f)) ||
        !FpsLogic::requestGrenadeProjectileImpact(registry, second, glm::vec3(5.0f)))
        return fail(name, "overlap fixture could not report server impacts");

    const auto detonated = FpsLogic::tickGrenadeEffects(registry, 0.01f, true);
    if (detonated.created_effects.size() != 2 ||
        registry.get<FpsHealthComponent>(owner).alive ||
        grenadeEffectCount(registry) != 2)
        return fail(name, "owner death did not preserve two authoritative effects");
    const auto& first_effect = registry.get<FpsLogic::FpsGrenadeEffectComponent>(detonated.created_effects[0]);
    const auto& second_effect = registry.get<FpsLogic::FpsGrenadeEffectComponent>(detonated.created_effects[1]);
    if (first_effect.owner != owner || second_effect.owner != owner ||
        first_effect.kind != FpsLogic::FpsGrenadeEffectKind::Smoke ||
        second_effect.kind != FpsLogic::FpsGrenadeEffectKind::Flash)
        return fail(name, "overlap effect order or owner attribution was not stable");

    FpsLogic::tickGrenadeEffects(registry, 0.20f, true);
    if (grenadeEffectCount(registry) != 1)
        return fail(name, "overlapping effects did not retain their independent lifetimes");
    FpsLogic::tickGrenadeEffects(registry, 0.10f, false);
    if (!registry.view<FpsLogic::FpsGrenadeEffectComponent>().empty())
        return fail(name, "round cleanup did not remove surviving overlapping effect");
    return pass(name);
}
