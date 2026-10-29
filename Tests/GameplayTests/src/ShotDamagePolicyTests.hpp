#pragma once

// Exercise the same lazy policy selection that the server's pellet-hit path
// uses, followed by its existing damage and pawn-respawn boundaries.
bool testShotDamagePolicyModeIsolation()
{
    const std::string name = "ShotDamagePolicy_ModeIsolation";
    using FpsServer::DamageRejectReason;

    for (const auto mode : {FpsLogic::ServerMode::Deathmatch, FpsLogic::ServerMode::TeamDeathmatch}) {
        world game_world;
        auto& registry = game_world.registry;
        const auto composition = FpsLogic::ModeComposition{mode};
        Game::GameRules rules;
        rules.configureMode(composition);

        auto makePlayer = [&](uint16_t client, uint8_t team) {
            const auto entity = registry.create();
            registry.emplace<Net::NetworkedEntity>(entity, 1000u + client, client, true);
            registry.emplace<TransformComponent>(entity, 0.0f, 0.0f, 0.0f);
            registry.emplace<FpsHealthComponent>(entity, 100);
            registry.emplace<TeamComponent>(entity, TeamComponent{team});
            registry.emplace<ScoreComponent>(entity);
            registry.emplace<InventoryComponent>(entity);
            registry.emplace<PlayerLoadoutComponent>(entity);
            registry.emplace<WeaponComponent>(entity);
            return entity;
        };
        const auto attacker = makePlayer(1, FpsLogic::kTeamAttackers);
        const auto teammate = makePlayer(3, FpsLogic::kTeamAttackers);
        const auto victim = makePlayer(2, FpsLogic::kTeamDefenders);
        if (registry.ctx().find<Competitive::CompetitiveWorldContext>() != nullptr)
            return fail(name, "score-mode fixture unexpectedly has competitive context");

        int phase_reads = 0;
        auto phase = [&] {
            ++phase_reads;
            const auto* context = registry.ctx().find<Competitive::CompetitiveWorldContext>();
            return context ? context->round.state.phase : Competitive::RoundPhase::Freeze;
        };
        auto hit = [&](entt::entity target, int32_t damage) {
            const auto policy = FpsServer::makeShotDamagePolicy(
                composition.usesCompetitiveSystems(), rules.match.config.mode, phase);
            return FpsServer::applyFpsDamage(registry, {attacker, target, damage}, policy);
        };

        const auto friendly = hit(teammate, 25);
        if (mode == FpsLogic::ServerMode::Deathmatch) {
            if (!friendly.applied || registry.get<FpsHealthComponent>(teammate).health != 75)
                return fail(name, "deathmatch rejected a valid same-team hit");
        } else if (friendly.applied || friendly.reject_reason != DamageRejectReason::SameTeam ||
                   registry.get<FpsHealthComponent>(teammate).health != 100) {
            return fail(name, "team deathmatch accepted same-team damage");
        }

        const auto lethal = hit(victim, 100);
        const auto transition = FpsServer::beginFpsDeathTransition(registry, victim);
        const auto duplicate = FpsServer::beginFpsDeathTransition(registry, victim);
        const auto score = rules.onPlayerKilled(game_world, 1, 2);
        bool restored = false;
        rules.setOnRespawn([&](uint16_t client, const glm::vec3&) {
            if (client == 2)
                restored = FPSShooter::restorePawnAfterRespawn(registry, victim, WeaponType::RIFLE);
        });
        rules.queueRespawn(2);
        rules.Update(game_world, rules.match.config.respawn_delay + 0.01f);
        if (!lethal.applied || !lethal.killed || !transition.transitioned || duplicate.transitioned ||
            !score.match_kill_recorded || rules.match.scoreOf(1) != 1 || !restored ||
            !registry.get<FpsHealthComponent>(victim).alive ||
            registry.get<FpsHealthComponent>(victim).health != 100 ||
            registry.get<FpsDeathLifecycleComponent>(victim).resolved || phase_reads != 0 ||
            registry.ctx().find<Competitive::CompetitiveWorldContext>() != nullptr) {
            return fail(name, "score-mode hit, death, or respawn touched competitive phase or lost state");
        }
    }

    entt::registry registry;
    const auto attacker = registry.create();
    const auto victim = registry.create();
    const auto teammate = registry.create();
    for (const auto entity : {attacker, victim, teammate})
        registry.emplace<FpsHealthComponent>(entity, 100);
    registry.emplace<TeamComponent>(attacker, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<TeamComponent>(victim, TeamComponent{FpsLogic::kTeamDefenders});
    registry.emplace<TeamComponent>(teammate, TeamComponent{FpsLogic::kTeamAttackers});
    auto& context = registry.ctx().emplace<Competitive::CompetitiveWorldContext>();
    int phase_reads = 0;
    auto phase = [&] { ++phase_reads; return context.round.state.phase; };
    auto hit = [&](entt::entity target) {
        const auto policy = FpsServer::makeShotDamagePolicy(
            true, FpsLogic::FpsGameMode::Deathmatch, phase);
        return FpsServer::applyFpsDamage(registry, {attacker, target, 25}, policy);
    };

    context.round.state.phase = Competitive::RoundPhase::Freeze;
    const auto frozen = hit(victim);
    context.round.state.phase = Competitive::RoundPhase::Live;
    const auto friendly = hit(teammate);
    const auto live = hit(victim);
    if (frozen.applied || frozen.reject_reason != DamageRejectReason::PhaseBlocked ||
        friendly.applied || friendly.reject_reason != DamageRejectReason::SameTeam ||
        !live.applied || registry.get<FpsHealthComponent>(victim).health != 75 || phase_reads != 3)
        return fail(name, "competitive Freeze/Live or team policy changed");

    return pass(name);
}
