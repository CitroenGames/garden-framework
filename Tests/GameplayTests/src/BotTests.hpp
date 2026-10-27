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

bool testBotPerceptionPrivacyAndStaleTargets()
{
    const std::string name = "Bot_PerceptionPrivacyAndStaleTargets";
    using namespace Bots;
    entt::registry registry;
    const entt::entity first = registry.create();
    const entt::entity second = registry.create();
    const glm::vec3 eye(0.0f, 1.62f, 0.0f);
    const glm::vec3 shared_position(0.0f, 0.0f, 10.0f);
    const auto clear = [](const glm::vec3&, const glm::vec3&) { return false; };

    // Equal visibility must not select a target by view iteration order.
    BotPerceptComponent ordered;
    updatePerception(ordered, eye, 0.0f,
                     {{second, shared_position}, {first, shared_position}}, 1, 0.1f, clear);
    if (ordered.visible_enemy != first)
        return fail(name, "equal-visibility target selection was not entity-stable");

    // A different visible target starts a fresh reaction interval. The bot
    // cannot combine visibility time from two different enemies.
    for (int i = 0; i < 3; ++i)
        updatePerception(ordered, eye, 0.0f, {{first, shared_position}}, 1, 0.1f, clear);
    if (!enemyConfirmed(ordered, 1))
        return fail(name, "baseline target did not confirm");
    updatePerception(ordered, eye, 0.0f, {{second, shared_position}}, 1, 0.1f, clear);
    if (enemyConfirmed(ordered, 1))
        return fail(name, "reaction timer leaked across target switch");

    // Lifecycle invalidation clears identity and remembered position immediately.
    pruneCurrentSight(ordered, false);
    if (ordered.visible_enemy != entt::null || ordered.visible_fraction != 0.0f ||
        ordered.confirm_timer != 0.0f || hasSightMemory(ordered, 1))
        return fail(name, "stale target retained visible or remembered state");

    // Lost sight preserves an anonymous bounded memory only; it expires at the
    // configured difficulty memory duration and cannot be used as a target ID.
    BotPerceptComponent memory;
    updatePerception(memory, eye, 0.0f, {{first, shared_position}}, 1, 0.1f, clear);
    updatePerception(memory, eye, 0.0f, {}, 1, 0.1f, clear);
    if (memory.visible_enemy != entt::null || !hasSightMemory(memory, 1))
        return fail(name, "lost sight did not retain bounded anonymous memory");
    advancePerceptionClock(memory, 1, difficultyFor(1).memory_time);
    if (hasSightMemory(memory, 1) || memory.last_seen_pos != glm::vec3(0.0f))
        return fail(name, "anonymous sight memory did not expire");
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

bool testBotBoundedPathScheduling()
{
    const std::string name = "Bot_BoundedPathScheduling";
    using namespace Bots;

    // Oldest pending request wins. Equal-age requests use the stable bot
    // index, then owner id, rather than incidental registry-view ordering.
    const std::vector<PathScheduleRequest> selected = selectPathRequests({
        {9, 40009, 22}, {4, 40004, 18}, {2, 40002, 22}, {1, 40001, 22}, {3, 40003, 18}}, 3);
    if (selected.size() != 3)
        return fail(name, "scheduler did not enforce configured budget");
    if (selected[0].bot_index != 3 || selected[1].bot_index != 4 || selected[2].bot_index != 1)
        return fail(name, "scheduler order was not age then stable identity");

    NavAgentComponent nav;
    nav.repath_cooldown = 0.25f;
    setPath(nav, {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, 0.0f)}, glm::vec3(8.0f, 0.0f, 0.0f));
    nav.stuck = true;
    if (!pathGoalChanged(nav, glm::vec3(8.0f, 0.0f, 0.0f)))
        return fail(name, "stuck bot did not request a safe re-path");
    queuePathRequest(nav, 77);
    queuePathRequest(nav, 78);
    if (!nav.path_request_pending || nav.path_request_tick != 77)
        return fail(name, "request age changed while already queued");

    // No path result must stop movement and back off rather than retrying
    // every tick or keeping a stale path alive.
    rejectPathRequest(nav);
    if (nav.path_request_pending || nav.path_valid || nav.has_goal || nav.desired_velocity != glm::vec3(0.0f))
        return fail(name, "failed path request retained movement state");
    if (nav.path_failures != 1 || nav.repath_timer <= nav.repath_cooldown)
        return fail(name, "failed path request did not apply bounded backoff");

    SteeringComponent steering;
    steering.last_pos = glm::vec3(3.0f, 0.0f, 0.0f);
    steering.has_last = true;
    acceptPathRequest(nav, {glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(6.0f, 0.0f, 0.0f)},
                      glm::vec3(6.0f, 0.0f, 0.0f), steering);
    if (!nav.path_valid || !nav.has_goal || nav.stuck || nav.path_request_pending ||
        !approx(nav.repath_timer, nav.repath_cooldown))
        return fail(name, "accepted path did not reset request and stuck state");
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

bool testBotEngageStrafes()
{
    const std::string name = "Bot_EngageStrafes";
    using namespace Bots;
    // Normal (strafe_interval 0.9): Engage with a visible enemy must produce
    // lateral movement while aiming, and the direction must flip over time.
    BotComponent b; b.difficulty = 1; b.state = BotState::Engage; b.rng_state = 99;
    NavAgentComponent nav; nav.desired_velocity = glm::vec3(0);
    BotIntentComponent intent; intent.want_attack = true; intent.aim_yaw = 0.0f;
    bool saw_left = false, saw_right = false, saw_move = false;
    for (int i = 0; i < 300; ++i) {
        Net::InputSample s = synthesizeInput(b, nav, intent, (uint32_t)i, 0.1f);
        if (s.move_right > 0.1f) saw_right = true;
        if (s.move_right < -0.1f) saw_left = true;
        if (s.move_right != 0.0f) saw_move = true;
        if (!(s.buttons & Net::InputFlags::ATTACK)) return fail(name, "Engage strafe lost ATTACK");
    }
    if (!saw_move) return fail(name, "Engage bot stationary while aiming (no strafe)");
    if (!saw_left || !saw_right) return fail(name, "strafe direction never flipped");
    // Easy (strafe_interval 0): standing still while aiming is by design.
    BotComponent easy; easy.difficulty = 0; easy.state = BotState::Engage; easy.rng_state = 5;
    Net::InputSample es = synthesizeInput(easy, nav, intent, 1, 0.1f);
    if (es.move_right != 0.0f) return fail(name, "Easy bot strafed despite strafe_interval=0");
    // No visible enemy (want_attack false): no strafe.
    BotComponent blind; blind.difficulty = 1; blind.state = BotState::Engage; blind.rng_state = 5;
    BotIntentComponent no_atk; no_atk.aim_yaw = 0.0f;
    Net::InputSample bs = synthesizeInput(blind, nav, no_atk, 1, 0.1f);
    if (bs.move_right != 0.0f) return fail(name, "strafed without a visible enemy");
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

bool testBotDebugRingWraps()
{
    const std::string name = "Bot_DebugRingWraps64";
    using namespace Bots;
    TransitionRingBuffer ring;
    for (uint32_t i = 0; i < 100; ++i) ring.push(TransitionRecord{i, 1, BotState::Idle, BotState::Advance, "test"});
    if (ring.size() != 64) return fail(name, "size not capped at 64");
    if (ring.oldest().tick != 36 || ring.newest().tick != 99) return fail(name, "wrap did not keep newest 64");
    if (ring.at(0).tick != 36 || ring.at(63).tick != 99) return fail(name, "at() ordering wrong");
    return pass(name);
}
bool testBotDebugTransitionRecorded()
{
    const std::string name = "Bot_DebugTransitionRecorded";
    using namespace Bots; using namespace Competitive;
    TransitionRingBuffer ring;
    BotComponent b; b.difficulty = 1; b.state = BotState::Advance; b.rng_state = 42; b.bot_index = 3;
    BotPerceptComponent p; BotIntentComponent intent;
    BotDecisionInput in; in.phase = RoundPhase::Live; in.team = FpsTeam::Attackers; in.is_carrier = true; in.at_site = true; in.has_site = true; in.site_pos = glm::vec3(10,0,0); in.dt = 0.1f;
    decide(b, p, intent, in, &ring, 123);
    if (ring.size() != 1) return fail(name, "transition not recorded");
    const auto& r = ring.newest();
    if (r.tick != 123 || r.bot_index != 3 || r.from != BotState::Advance || r.to != BotState::Plant || r.reason != "carrier_at_site") return fail(name, "record fields wrong");
    // No ring: unchanged behaviour, no crash.
    BotComponent b2 = b; BotIntentComponent i2; decide(b2, p, i2, in);
    if (b2.state != BotState::Plant) return fail(name, "null-ring decide changed state");
    return pass(name);
}
bool testBotDebugDumpFormatter()
{
    const std::string name = "Bot_DebugDumpFormatter";
    using namespace Bots;
    BotDumpInput d; d.bot_index = 2; d.team = 1; d.state = BotState::Advance; d.hp = 77; d.pos = glm::vec3(1,2,3); d.goal = glm::vec3(4,0,5); d.has_goal = true; d.path_valid = true; d.stuck = false; d.visible_age = 0.5f; d.heard_age = 2.0f;
    std::string s = formatBotDump(d);
    for (const char* k : {"[BotDebug]", "bot=2", "team=1", "state=Advance", "hp=77", "pos=", "goal=", "path_valid=1", "stuck=0", "percept_visible_age=", "percept_heard_age="}) if (s.find(k) == std::string::npos) return fail(name, std::string("missing key: ") + k);
    if (s.find('\n') != std::string::npos) return fail(name, "dump not single-line");
    return pass(name);
}
bool testBotDebugPausedNeutral()
{
    const std::string name = "Bot_DebugPausedNeutral";
    using namespace Bots;
    Net::InputSample s = synthesizeNeutralInput(9, 0.5f, -0.2f);
    if (s.tick != 9 || s.buttons != 0 || s.move_forward != 0.0f || s.move_right != 0.0f) return fail(name, "paused sample not neutral");
    if (!botApprox(s.camera_yaw, 0.5f) || !botApprox(s.camera_pitch, -0.2f)) return fail(name, "paused camera not preserved");
    StuckLogLatch latch; if (!latch.check(true) || latch.check(true) || latch.check(false) || !latch.check(true)) return fail(name, "stuck latch not one-shot per episode");
    return pass(name);
}
bool testBombAuthoredSitesParse()
{
    const std::string name = "Bomb_AuthoredSitesParse";
    using namespace Competitive;
    std::vector<BombSite> sites; std::string err;
    const std::string good = "{\"sites\":[{\"name\":\"A\",\"centre\":[30.0,5.0,15.0],\"radius\":10.0},{\"name\":\"B\",\"centre\":[-30.0,5.0,-15.0],\"radius\":10.0}]}";
    if (!parseAuthoredBombSites(good, sites, &err)) return fail(name, "valid authored sites rejected: " + err);
    if (sites.size() != 2 || sites[0].site_index != 0 || sites[1].site_index != 1) return fail(name, "site indices wrong");
    if (glm::distance(sites[0].centre, glm::vec3(30,5,15)) > 0.01f) return fail(name, "site A centre wrong");
    BombConfig cfg; cfg.sites = sites; uint8_t idx = 99;
    if (!cfg.inSite(glm::vec3(30,5,15), idx) || idx != 0) return fail(name, "inSite A failed");
    if (cfg.inSite(glm::vec3(0,5,0), idx)) return fail(name, "origin unexpectedly in a site");
    if (parseAuthoredBombSites("{\"sites\":[{\"centre\":[0,0,0],\"radius\":10}]}", sites, &err)) return fail(name, "single site accepted");
    if (parseAuthoredBombSites("{\"sites\":[{\"centre\":[0,0,0],\"radius\":1},{\"centre\":[50,0,0],\"radius\":10}]}", sites, &err)) return fail(name, "tiny radius accepted");
    if (parseAuthoredBombSites("not json", sites, &err)) return fail(name, "garbage accepted");
    if (parseAuthoredBombSites("{\"sites\":[{\"centre\":[0,0,0],\"radius\":10},{\"centre\":[1,0,0],\"radius\":10}]}", sites, &err)) return fail(name, "overlapping sites accepted");
    return pass(name);
}
