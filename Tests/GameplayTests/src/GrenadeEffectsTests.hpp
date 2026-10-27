// C4 grenade-effect foundation tests included by GameplayTests main.cpp.
#include "shared/GrenadeEffects.hpp"

#include <string>

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
