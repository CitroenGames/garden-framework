#pragma once
#include "shared/BotLogic.hpp"
#include <cmath>
#include <string>
namespace {
bool botApprox(float a, float b, float e = 0.001f) { return std::abs(a - b) <= e; }
} // namespace

bool testBotDifficultyClampTable()
{
    const std::string name = "Bot_DifficultyClampTable";
    using namespace Bots;
    if (clampDifficulty(-5) != 0 || clampDifficulty(99) != 3 || clampDifficulty(2) != 2) return fail(name, "clamp wrong");
    if (!botApprox(difficultyFor(0).reaction_time, 0.60f) || !botApprox(difficultyFor(1).reaction_time, 0.40f) ||
        !botApprox(difficultyFor(2).reaction_time, 0.25f) || !botApprox(difficultyFor(3).reaction_time, 0.15f)) return fail(name, "reaction row wrong");
    if (!botApprox(difficultyFor(0).aim_error_deg, 6.0f) || !botApprox(difficultyFor(3).aim_error_deg, 0.8f)) return fail(name, "aim error wrong");
    if (!botApprox(difficultyFor(3).view_distance, 90.0f) || !botApprox(difficultyFor(3).fov_deg, 120.0f) || difficultyFor(3).burst_len != 6 || difficultyFor(3).retreat_hp != 35) return fail(name, "expert row wrong");
    if (!botApprox(difficultyFor(99).reaction_time, 0.15f)) return fail(name, "unknown difficulty not clamped to Expert range via [0,3]");
    // Unknown byte stored on component is clamped at read time.
    if (!botApprox(difficultyFor(250).aim_turn_rate_deg, 540.0f)) return fail(name, "unknown clamp turn rate wrong");
    return pass(name);
}

bool testBotPerceptionBlindfold()
{
    const std::string name = "Bot_PerceptionBlindfold";
    using namespace Bots;
    entt::registry reg; entt::entity enemy = reg.create();
    std::vector<EnemyCandidate> enemies{{enemy, glm::vec3(0, 0, 10)}};
    // Behind wall: every sample blocked.
    BotPerceptComponent p;
    for (int i = 0; i < 50; ++i) updatePerception(p, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](const glm::vec3&, const glm::vec3&) { return true; });
    if (p.visible_fraction != 0.0f || p.visible_enemy != entt::null || enemyConfirmed(p, 1)) return fail(name, "walled enemy leaked into percept");
    // Visible: confirms only after Normal reaction 0.40s.
    BotPerceptComponent p2;
    updatePerception(p2, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](const glm::vec3&, const glm::vec3&) { return false; });
    if (p2.visible_fraction != 1.0f || enemyConfirmed(p2, 1)) return fail(name, "confirmed before reaction_time");
    for (int i = 0; i < 3; ++i) updatePerception(p2, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](const glm::vec3&, const glm::vec3&) { return false; });
    if (!enemyConfirmed(p2, 1)) return fail(name, "not confirmed after reaction_time (0.4s)");
    if (p2.last_seen_age != 0.0f) return fail(name, "last_seen_age not reset");
    // Behind bot: FOV rejects (dist > 3).
    BotPerceptComponent p3; std::vector<EnemyCandidate> behind{{enemy, glm::vec3(0, 0, -10)}};
    updatePerception(p3, glm::vec3(0, 1.62f, 0), 0.0f, behind, 1, 0.1f, [](const glm::vec3&, const glm::vec3&) { return false; });
    if (p3.visible_fraction != 0.0f) return fail(name, "enemy behind bot visible");
    return pass(name);
}

bool testBotFsmPlantDefendDefuse()
{
    const std::string name = "Bot_FsmPlantDefendDefuse";
    using namespace Bots; using namespace Competitive;
    // T carrier: Advance -> Plant -> Defend
    BotComponent b; b.difficulty = 1; b.state = BotState::Advance; b.rng_state = 42;
    BotPerceptComponent p; BotIntentComponent intent;
    BotDecisionInput in; in.phase = RoundPhase::Live; in.team = FpsTeam::Attackers; in.is_carrier = true; in.at_site = true; in.has_site = true; in.site_pos = glm::vec3(10, 0, 0); in.dt = 0.1f;
    decide(b, p, intent, in);
    if (b.state != BotState::Plant || !intent.want_use) return fail(name, "carrier at site did not Plant with USE");
    in.bomb_planted = true; decide(b, p, intent, in);
    if (b.state != BotState::Defend) return fail(name, "planted carrier did not Defend");
    // CT: planted, at bomb -> Defuse with USE
    BotComponent ct; ct.difficulty = 1; ct.state = BotState::Advance;
    BotDecisionInput cin; cin.phase = RoundPhase::Live; cin.team = FpsTeam::Defenders; cin.bomb_planted = true; cin.at_bomb = true; cin.has_bomb_pos = true; cin.bomb_pos = glm::vec3(1, 0, 0); cin.dt = 0.1f;
    decide(ct, p, intent, cin);
    if (ct.state != BotState::Defuse || !intent.want_use) return fail(name, "CT at bomb did not Defuse with USE");
    // Death -> Spectate
    BotComponent dead; dead.state = BotState::Advance; BotDecisionInput din = cin; din.alive = false;
    decide(dead, p, intent, din);
    if (dead.state != BotState::Spectate) return fail(name, "death did not Spectate");
    // Engage overlay + resume
    BotComponent e; e.difficulty = 1; e.state = BotState::Advance; e.rng_state = 7;
    BotPerceptComponent ep; ep.visible_fraction = 1.0f; ep.confirm_timer = 1.0f; ep.last_seen_pos = glm::vec3(0, 0, 10);
    BotDecisionInput ein; ein.phase = RoundPhase::Live; ein.team = FpsTeam::Attackers; ein.has_site = true; ein.site_pos = glm::vec3(20, 0, 0); ein.dt = 0.1f;
    decide(e, ep, intent, ein);
    if (e.state != BotState::Engage || !e.has_resume || e.resume_state != BotState::Advance) return fail(name, "confirmed enemy did not Engage with resume=Advance");
    if (!intent.want_attack) return fail(name, "Engage did not want attack");
    ep.visible_fraction = 0.0f; ep.confirm_timer = 0.0f;
    for (int i = 0; i < 10; ++i) decide(e, ep, intent, ein);
    if (e.state != BotState::Advance) return fail(name, "lost enemy did not resume Advance");
    return pass(name);
}

bool testBotSteeringArrivalStuck()
{
    const std::string name = "Bot_SteeringArrivalStuck";
    using namespace Bots;
    NavAgentComponent nav; SteeringComponent st;
    setPath(nav, {glm::vec3(0, 0, 0), glm::vec3(5, 0, 0)}, glm::vec3(5, 0, 0));
    glm::vec3 pos(0, 0, 0); glm::vec3 start = pos;
    for (int i = 0; i < 600 && nav.has_goal; ++i) { updateSteering(nav, st, pos, 1.0f / 60.0f); pos += st.velocity * (1.0f / 60.0f); }
    if (nav.has_goal) return fail(name, "did not arrive at final waypoint");
    if (glm::distance(pos, glm::vec3(5, 0, 0)) > 1.0f) return fail(name, "arrival pos too far");
    if (glm::distance(pos, start) < 3.0f) return fail(name, "bot did not move (teleport check baseline)");
    // Stuck: path ahead but position frozen.
    NavAgentComponent nav2; SteeringComponent st2; setPath(nav2, {glm::vec3(0, 0, 0), glm::vec3(10, 0, 0)}, glm::vec3(10, 0, 0));
    glm::vec3 frozen(0, 0, 0);
    for (int i = 0; i < 120; ++i) updateSteering(nav2, st2, frozen, 1.0f / 60.0f);
    if (!nav2.stuck) return fail(name, "stuck not flagged when frozen with commanded velocity");
    if (frozen != glm::vec3(0, 0, 0)) return fail(name, "steering teleported frozen bot");
    return pass(name);
}

bool testBotInputSynthesisShape()
{
    const std::string name = "Bot_InputSynthesisShape";
    using namespace Bots;
    BotComponent b; b.difficulty = 1; b.state = BotState::Advance; b.rng_state = 123;
    NavAgentComponent nav; nav.desired_velocity = glm::vec3(0, 0, 5);
    BotIntentComponent intent; intent.aim_yaw = 0.0f;
    Net::InputSample s = synthesizeInput(b, nav, intent, 42, 1.0f / 60.0f);
    if (s.tick != 42) return fail(name, "tick not copied");
    if (s.move_forward < -1.0f || s.move_forward > 1.0f || s.move_right < -1.0f || s.move_right > 1.0f) return fail(name, "axes out of [-1,1]");
    if (s.move_forward < 0.9f) return fail(name, "forward velocity did not map to +move_forward in view frame");
    if (!(s.buttons & Net::InputFlags::MOVE_FORWARD)) return fail(name, "MOVE_FORWARD button missing");
    // Plant hold: USE set, movement zeroed.
    BotComponent pb; pb.state = BotState::Plant; NavAgentComponent pnav; pnav.desired_velocity = glm::vec3(5, 0, 0);
    BotIntentComponent pint; pint.want_use = true;
    Net::InputSample ps = synthesizeInput(pb, pnav, pint, 7, 1.0f / 60.0f);
    if (!(ps.buttons & Net::InputFlags::USE) || ps.move_forward != 0.0f || ps.move_right != 0.0f) return fail(name, "plant USE hold wrong");
    return pass(name);
}

bool testBotBuyPlanByMoney()
{
    const std::string name = "Bot_BuyPlanByMoney";
    using namespace Bots; using namespace Competitive;
    auto full = selectBuyPlan(5000, FpsTeam::Attackers, 1, 0);
    if (full.kind != BuyPlanKind::Full || full.aliases.empty() || std::string(full.aliases[0]) != "ak47") return fail(name, "full buy wrong");
    auto fullCt = selectBuyPlan(5000, FpsTeam::Defenders, 1, 0);
    bool hasKit = false; for (auto* a : fullCt.aliases) if (std::string(a) == "defuser") hasKit = true;
    if (fullCt.aliases.empty() || std::string(fullCt.aliases[0]) != "m4a1" || !hasKit) return fail(name, "CT full buy wrong (rifle/kit)");
    auto force = selectBuyPlan(2000, FpsTeam::Attackers, 1, 0);
    if (force.kind != BuyPlanKind::Force || force.aliases.empty() || std::string(force.aliases[0]) != "mac10") return fail(name, "force buy wrong");
    auto eco = selectBuyPlan(400, FpsTeam::Attackers, 1, 2);
    if (eco.kind != BuyPlanKind::Eco || eco.aliases.empty() || std::string(eco.aliases[0]) != "p250") return fail(name, "eco buy wrong");
    auto pistol = selectBuyPlan(800, FpsTeam::Attackers, 1, 0);
    if (pistol.kind != BuyPlanKind::Pistol || !pistol.aliases.empty()) return fail(name, "pistol round Normal should save (no vest below Hard)");
    auto pistolHard = selectBuyPlan(800, FpsTeam::Attackers, 2, 0);
    if (pistolHard.aliases.empty() || std::string(pistolHard.aliases[0]) != "vest") return fail(name, "pistol Hard should buy vest");
    // Every alias must resolve in catalog (server skip-on-reject relies on it).
    for (auto* plan : {&full, &fullCt, &force, &eco}) for (auto* a : plan->aliases) if (!findCatalogByAlias(a)) return fail(name, std::string("alias not in catalog: ") + a);
    return pass(name);
}
