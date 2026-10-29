// Compile the actual server implementation into this test target. The guarded
// entry point calls its static fireServerShot; the shipping module has no hook.
#include "server/ServerModule.cpp"

#include <iostream>

static bool runCase(FpsLogic::ServerMode mode, bool friendly,
                    Competitive::RoundPhase phase, bool expect_damage,
                    const char* name)
{
    world game_world;
    game_world.initializePhysics();
    auto& registry = game_world.registry;
    registry.ctx().emplace<Bots::BotWorldRuntime>();
    if (mode == FpsLogic::ServerMode::Competitive)
        registry.ctx().emplace<Competitive::CompetitiveWorldContext>().round.state.phase = phase;

    auto player = [&](uint16_t client, uint8_t team, const glm::vec3& position) {
        const entt::entity entity = registry.create();
        registry.emplace<NetworkedEntity>(entity, 1000u + client, client, true);
        registry.emplace<TransformComponent>(entity, position.x, position.y, position.z);
        registry.emplace<FpsHealthComponent>(entity, 100);
        registry.emplace<TeamComponent>(entity, TeamComponent{team});
        return entity;
    };
    const auto shooter = player(1, FpsLogic::kTeamAttackers, glm::vec3(0.0f));
    const auto victim = player(2, friendly ? FpsLogic::kTeamAttackers : FpsLogic::kTeamDefenders,
                               glm::vec3(0.0f, 0.0f, 10.0f));
    EngineServices services{};
    services.game_world = &game_world;
    InputSample input{};
    const auto misses_before = g_lag_compensation_telemetry.target_history_sample_misses;
    const auto sequence_before = g_presentation_event_sequences[1];
    fireServerShotForTest(services, mode, shooter, input);
    const int health_after_hit = registry.get<FpsHealthComponent>(victim).health;
    const bool hit_ok = health_after_hit == (expect_damage ? 100 - getWeaponDef(WeaponType::RIFLE).damage : 100);

    // The same eligible target remains in the world but is moved off the ray.
    registry.get<TransformComponent>(victim).position.x = 10.0f;
    fireServerShotForTest(services, mode, shooter, input);
    const bool miss_ok = registry.get<FpsHealthComponent>(victim).health == health_after_hit;
    const bool shot_paths_entered = g_lag_compensation_telemetry.target_history_sample_misses == misses_before + 2 &&
        g_presentation_event_sequences[1] >= sequence_before + 2;
    const bool context_ok = mode == FpsLogic::ServerMode::Competitive ||
        registry.ctx().find<Competitive::CompetitiveWorldContext>() == nullptr;
    const bool ok = hit_ok && miss_ok && shot_paths_entered && context_ok;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    return ok;
}

int main()
{
    EE::CLog::Init();
    using FpsLogic::ServerMode;
    using Competitive::RoundPhase;
    bool ok = true;
    ok = runCase(ServerMode::Deathmatch, false, RoundPhase::Freeze, true, "Deathmatch_HitAndMiss") && ok;
    ok = runCase(ServerMode::Deathmatch, true, RoundPhase::Freeze, true, "Deathmatch_FriendlyHitAndMiss") && ok;
    ok = runCase(ServerMode::TeamDeathmatch, false, RoundPhase::Freeze, true, "TeamDeathmatch_HitAndMiss") && ok;
    ok = runCase(ServerMode::TeamDeathmatch, true, RoundPhase::Freeze, false, "TeamDeathmatch_FriendlyHitAndMiss") && ok;
    ok = runCase(ServerMode::Competitive, false, RoundPhase::Freeze, false, "Competitive_FreezeHitAndMiss") && ok;
    ok = runCase(ServerMode::Competitive, false, RoundPhase::Live, true, "Competitive_LiveHitAndMiss") && ok;
    ok = runCase(ServerMode::Competitive, true, RoundPhase::Live, false, "Competitive_FriendlyHitAndMiss") && ok;
    return ok ? 0 : 1;
}
