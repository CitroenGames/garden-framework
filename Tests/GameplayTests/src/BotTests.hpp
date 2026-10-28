#pragma once
#include "shared/BotLogic.hpp"
#include "shared/BotRouteData.hpp"
#include "shared/BotNavigationRuntime.hpp"
#include <cmath>
#include <limits>
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

bool testBotHearingRangeSelectionAndBlur()
{
    const std::string name = "Bot_HearingRangeSelectionAndBlur";
    using namespace Bots;
    const glm::vec3 listener(0.0f);
    BotHearingCandidate boundary{{31.5f, 0.0f, 0.0f}, 100, 1, 1.0f,
                                 BotHeardNoiseCategory::WeaponFire};
    if (!isHearingCandidateEligible(boundary, listener, 0, 100, 0))
        return fail(name, "easy difficulty rejected its exact radius boundary");
    boundary.origin.x += 0.01f;
    if (isHearingCandidateEligible(boundary, listener, 0, 100, 0))
        return fail(name, "easy difficulty accepted outside its scaled radius");
    if (!isHearingCandidateEligible(boundary, listener, 1, 100, 0))
        return fail(name, "normal difficulty did not retain the broader range");

    const BotHearingCandidate quieter{{4.0f, 0.0f, 0.0f}, 90, 3, 0.80f,
                                      BotHeardNoiseCategory::WeaponFire};
    const BotHearingCandidate older{{5.0f, 0.0f, 0.0f}, 89, 7, 0.90f,
                                    BotHeardNoiseCategory::WeaponFire};
    const BotHearingCandidate newer{{6.0f, 0.0f, 0.0f}, 95, 4, 0.90f,
                                    BotHeardNoiseCategory::WeaponFire};
    const BotHearingCandidate serial_tie{{7.0f, 0.0f, 0.0f}, 95, 8, 0.90f,
                                         BotHeardNoiseCategory::WeaponFire};
    BotHearingCandidate first;
    BotHearingCandidate second;
    if (!selectHearingCandidate({quieter, older, newer, serial_tie}, listener, 1, 100, 0, first) ||
        !selectHearingCandidate({serial_tie, newer, older, quieter}, listener, 1, 100, 0, second) ||
        first.serial != 8 || second.serial != first.serial)
        return fail(name, "hearing selection was not loudness then tick then serial stable");

    const glm::vec3 blur_a = blurredHearingPosition(serial_tie.origin, serial_tie.serial, 3,
                                                    serial_tie.category);
    const glm::vec3 blur_repeat = blurredHearingPosition(serial_tie.origin, serial_tie.serial, 3,
                                                         serial_tie.category);
    const glm::vec3 blur_other = blurredHearingPosition(serial_tie.origin, serial_tie.serial, 4,
                                                        serial_tie.category);
    const float cap = hearingProfile(serial_tie.category).blur_radius;
    if (!isFiniteBotVector(blur_a) || blur_a != blur_repeat ||
        glm::distance(blur_a, serial_tie.origin) > cap + 0.001f ||
        glm::distance(blur_other, serial_tie.origin) > cap + 0.001f)
        return fail(name, "deterministic blur escaped its finite category cap");
    return pass(name);
}

bool testBotHearingExpiryOcclusionAndWorldReset()
{
    const std::string name = "Bot_HearingExpiryOcclusionAndWorldReset";
    using namespace Bots;
    BotPerceptComponent percept;
    const BotHearingCandidate fire{{8.0f, 0.0f, 0.0f}, 10, 4, 0.75f,
                                   BotHeardNoiseCategory::WeaponFire};
    if (!applyHearingObservation(percept, fire, glm::vec3(7.5f, 0.0f, 0.0f), fire.loudness) ||
        !hasFreshHearing(percept))
        return fail(name, "open hearing observation was not retained");
    advanceHearingClock(percept, -1.0f);
    if (!botApprox(percept.hearing.age, 0.0f))
        return fail(name, "negative time changed hearing age");
    advanceHearingClock(percept, hearingProfile(fire.category).lifetime);
    if (hasFreshHearing(percept) || percept.hearing.category != BotHeardNoiseCategory::None)
        return fail(name, "hearing did not clear at its lifetime boundary");

    const float muffled = fire.loudness * kBotHearingBlockedMultiplier;
    if (!applyHearingObservation(percept, fire, glm::vec3(7.5f, 0.0f, 0.0f), muffled) ||
        !hasFreshHearing(percept))
        return fail(name, "blocked hearing above the threshold was rejected");
    clearHearing(percept);
    if (applyHearingObservation(percept, fire, glm::vec3(7.5f, 0.0f, 0.0f),
                                kBotHearingMinimumLoudness - 0.01f))
        return fail(name, "below-threshold blocked hearing became an observation");

    NoiseRing ring;
    for (uint32_t index = 0; index < NoiseRing::kCapacity + 1; ++index) {
        if (!ring.publish(BotHeardNoiseCategory::WeaponFire, glm::vec3(static_cast<float>(index), 0.0f, 0.0f),
                          index, 1.0f))
            return fail(name, "valid ring record was rejected");
    }
    const auto retained = ring.hearingCandidates();
    if (ring.size() != NoiseRing::kCapacity || ring.at(0).serial != 2 || ring.overwritten != 1 ||
        retained.empty() || retained.front().serial != 2)
        return fail(name, "ring did not deterministically evict its oldest record");
    BotWorldRuntime runtime;
    if (runtime.publishNoise(BotHeardNoiseCategory::WeaponFire, glm::vec3(0.0f), 1, 1.0f) ||
        runtime.noise_ring.rejected != 1)
        return fail(name, "stopped world runtime accepted a noise record");
    runtime.noise_ring = ring;
    runtime.reset();
    if (runtime.noise_ring.size() != 0 || runtime.noise_ring.accepted != 0 || runtime.noise_ring.next_serial != 1)
        return fail(name, "world reset retained hearing records or serial state");
    return pass(name);
}

bool testBotHearingBombProducerPolicy()
{
    const std::string name = "Bot_HearingBombProducerPolicy";
    using namespace Bots;
    using B = Competitive::BombState;
    if (completedBombNoiseCategory(B::Planting, B::Planted) != BotHeardNoiseCategory::BombPlanted ||
        completedBombNoiseCategory(B::Defusing, B::Defused) != BotHeardNoiseCategory::BombDefused ||
        completedBombNoiseCategory(B::Planted, B::Exploded) != BotHeardNoiseCategory::BombDetonated ||
        completedBombNoiseCategory(B::Defusing, B::Exploded) != BotHeardNoiseCategory::BombDetonated ||
        completedBombNoiseCategory(B::Carried, B::Planting) != BotHeardNoiseCategory::None ||
        completedBombNoiseCategory(B::Planting, B::Carried) != BotHeardNoiseCategory::None ||
        completedBombNoiseCategory(B::Dropped, B::Carried) != BotHeardNoiseCategory::None)
        return fail(name, "bomb hearing producer policy accepted a non-completed transition");
    NoiseRing ring;
    if (ring.publish(BotHeardNoiseCategory::WeaponFire, glm::vec3(std::numeric_limits<float>::infinity()), 1, 1.0f) ||
        ring.publish(BotHeardNoiseCategory::None, glm::vec3(0.0f), 1, 1.0f) ||
        ring.publish(BotHeardNoiseCategory::WeaponFire, glm::vec3(0.0f), 1, 0.0f) || ring.size() != 0)
        return fail(name, "rejected producer input entered the ring");
    return pass(name);
}

bool testBotHearingSightWinsAndInvestigatesOnly()
{
    const std::string name = "Bot_HearingSightWinsAndInvestigatesOnly";
    using namespace Bots;
    const BotHearingCandidate hearing_source{{11.0f, 0.0f, 4.0f}, 1, 9, 1.0f,
                                             BotHeardNoiseCategory::WeaponFire};
    BotDecisionInput input;
    input.phase = Competitive::RoundPhase::Live;
    input.team = Competitive::FpsTeam::Attackers;
    input.bot_pos = glm::vec3(0.0f);
    input.has_site = true;
    input.site_pos = glm::vec3(30.0f, 0.0f, 0.0f);

    BotPerceptComponent hearing_only;
    if (!applyHearingObservation(hearing_only, hearing_source, glm::vec3(10.0f, 0.0f, 3.0f), 1.0f))
        return fail(name, "hearing fixture could not create a value-only summary");
    BotComponent investigator;
    investigator.state = BotState::Advance;
    BotIntentComponent investigate_intent;
    TransitionRingBuffer transitions;
    decide(investigator, hearing_only, investigate_intent, input, &transitions, 55);
    if (!investigate_intent.has_move_goal || investigate_intent.move_goal != hearing_only.hearing.investigation_pos ||
        investigate_intent.want_attack || investigate_intent.want_use || investigator.state != BotState::Advance ||
        transitions.size() != 1 || transitions.newest().reason != "investigate_hearing")
        return fail(name, "hearing changed combat/use state instead of only supplying movement");
    input.phase = Competitive::RoundPhase::Freeze;
    input.has_site = false;
    decide(investigator, hearing_only, investigate_intent, input);
    if (investigate_intent.has_move_goal || investigate_intent.want_attack || investigate_intent.want_use)
        return fail(name, "freeze phase accepted a hearing investigation request");

    BotPerceptComponent sight_and_hearing = hearing_only;
    sight_and_hearing.visible_fraction = 1.0f;
    sight_and_hearing.confirm_timer = 1.0f;
    sight_and_hearing.last_seen_aim_point = glm::vec3(0.0f, kBotWeaponAimHeight, 12.0f);
    BotComponent engaged;
    engaged.state = BotState::Advance;
    input.phase = Competitive::RoundPhase::Live;
    BotIntentComponent engage_intent;
    decide(engaged, sight_and_hearing, engage_intent, input);
    if (engaged.state != BotState::Engage || !engage_intent.want_attack || engage_intent.has_move_goal)
        return fail(name, "confirmed sight did not win over hearing");
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
    for (int i = 0; i < 50; ++i) updatePerception(p, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](entt::entity, const glm::vec3&, const glm::vec3&) { return true; });
    if (p.visible_fraction != 0.0f || p.visible_enemy != entt::null || enemyConfirmed(p, 1)) return fail(name, "walled enemy leaked into percept");
    // Visible: confirms only after Normal reaction 0.40s.
    BotPerceptComponent p2;
    updatePerception(p2, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](entt::entity, const glm::vec3&, const glm::vec3&) { return false; });
    if (p2.visible_fraction != 1.0f || enemyConfirmed(p2, 1)) return fail(name, "confirmed before reaction_time");
    for (int i = 0; i < 3; ++i) updatePerception(p2, glm::vec3(0, 1.62f, 0), 0.0f, enemies, 1, 0.1f, [](entt::entity, const glm::vec3&, const glm::vec3&) { return false; });
    if (!enemyConfirmed(p2, 1)) return fail(name, "not confirmed after reaction_time (0.4s)");
    if (p2.last_seen_age != 0.0f) return fail(name, "last_seen_age not reset");
    // Behind bot: FOV rejects (dist > 3).
    BotPerceptComponent p3; std::vector<EnemyCandidate> behind{{enemy, glm::vec3(0, 0, -10)}};
    updatePerception(p3, glm::vec3(0, 1.62f, 0), 0.0f, behind, 1, 0.1f, [](entt::entity, const glm::vec3&, const glm::vec3&) { return false; });
    if (p3.visible_fraction != 0.0f) return fail(name, "enemy behind bot visible");
    return pass(name);
}

bool testBotPerceptionWeaponLineObstruction()
{
    const std::string name = "Bot_PerceptionWeaponLineObstruction";
    using namespace Bots;
    entt::registry registry;
    const entt::entity clear_enemy = registry.create();
    const entt::entity divider_enemy = registry.create();
    const glm::vec3 eye(0.0f, kBotWeaponOriginHeight, 0.0f);
    const glm::vec3 clear_position(0.0f, 0.0f, 10.0f);
    const glm::vec3 divider_position(4.0f, 0.0f, 10.0f);
    bool saw_canonical_aim_line = false;
    const auto weapon_trace = [&](entt::entity target, const glm::vec3& origin, const glm::vec3& point) {
        if (target == divider_enemy)
            return true; // central solid divider is the first weapon-trace hit.
        if (target == clear_enemy && origin == eye &&
            point == clear_position + glm::vec3(0.0f, kBotWeaponAimHeight, 0.0f))
            saw_canonical_aim_line = true;
        return false;
    };

    BotPerceptComponent percept;
    updatePerception(percept, eye, 0.0f,
                     {{divider_enemy, divider_position}, {clear_enemy, clear_position}},
                     1, 0.1f, weapon_trace);
    if (!saw_canonical_aim_line)
        return fail(name, "perception did not query the server weapon aim line");
    if (percept.visible_enemy != clear_enemy || percept.last_seen_aim_point !=
        clear_position + glm::vec3(0.0f, kBotWeaponAimHeight, 0.0f))
        return fail(name, "divider-blocked target entered bot percept");
    for (int i = 0; i < 3; ++i)
        updatePerception(percept, eye, 0.0f, {{clear_enemy, clear_position}}, 1, 0.1f, weapon_trace);
    BotComponent clear_bot;
    clear_bot.state = BotState::Advance;
    BotIntentComponent clear_intent;
    BotDecisionInput clear_input;
    clear_input.phase = Competitive::RoundPhase::Live;
    clear_input.team = Competitive::FpsTeam::Attackers;
    clear_input.bot_pos = glm::vec3(0.0f);
    clear_input.dt = 0.1f;
    decide(clear_bot, percept, clear_intent, clear_input);
    if (clear_bot.state != BotState::Engage || !clear_intent.want_attack)
        return fail(name, "clear weapon line did not authorize an engagement");

    BotPerceptComponent blocked_only;
    updatePerception(blocked_only, eye, 0.0f, {{divider_enemy, divider_position}},
                     1, 0.1f, weapon_trace);
    if (blocked_only.visible_enemy != entt::null || blocked_only.visible_fraction != 0.0f)
        return fail(name, "blocked weapon line entered bot percept");
    BotComponent blocked_bot;
    blocked_bot.state = BotState::Advance;
    BotIntentComponent blocked_intent;
    decide(blocked_bot, blocked_only, blocked_intent, clear_input);
    if (blocked_bot.state == BotState::Engage || blocked_intent.want_attack)
        return fail(name, "blocked weapon line authorized an engagement");
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
    const auto clear = [](entt::entity, const glm::vec3&, const glm::vec3&) { return false; };

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

bool testBotNavigationRecoveryValueRules()
{
    const std::string name = "Bot_NavigationRecoveryValueRules";
    using namespace Bots;
    if (std::string(navFailureCategoryName(NavFailureCategory::StartOffMesh)) != "start_off_mesh" ||
        std::string(navFailureCategoryName(NavFailureCategory::QueryCapacity)) != "query_capacity" ||
        std::string(localBlockDetailName(LocalBlockDetail::Crowd)) != "crowd")
        return fail(name, "stable failure labels changed");

    // Exercise the exact S4 branch that tickBots invokes.  It must not fire
    // early, then must make LocalBlock win on the 45th 60 Hz tick even though
    // the frozen steering window would also become eligible that tick.
    NavAgentComponent nav;
    SteeringComponent steer;
    setPath(nav, {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, 0.0f)}, glm::vec3(8.0f, 0.0f, 0.0f));
    const glm::vec3 frozen(0.0f);
    for (int i = 0; i < 44; ++i) {
        if (tickNavigationRecovery(nav, steer, frozen, LocalBlockDetail::Static,
                                   1.0f / 60.0f).category != NavFailureCategory::None)
            return fail(name, "static probe recovered before 45 fixed ticks");
    }
    const NavigationRecoveryResult local = tickNavigationRecovery(
        nav, steer, frozen, LocalBlockDetail::Static, 1.0f / 60.0f);
    if (local.category != NavFailureCategory::LocalBlock ||
        local.detail != LocalBlockDetail::Static || nav.last_failure != local.category ||
        nav.recovery_count != 1 || nav.path_valid || nav.has_goal ||
        nav.desired_velocity != glm::vec3(0.0f) ||
        nav.repath_timer <= nav.repath_cooldown || nav.local_block_detail != LocalBlockDetail::None)
        return fail(name, "local recovery did not win the shared fixed-tick interval");

    // A changed detail and clear probe restart confirmation. A continuous
    // crowd probe gets its own LocalBlock detail, never a second category.
    setPath(nav, {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, 0.0f)}, glm::vec3(8.0f, 0.0f, 0.0f));
    for (int i = 0; i < 44; ++i)
        if (observeLocalBlock(nav, LocalBlockDetail::Crowd, 1.0f / 60.0f))
            return fail(name, "crowd probe recovered early");
    if (observeLocalBlock(nav, LocalBlockDetail::Static, 1.0f / 60.0f) ||
        observeLocalBlock(nav, LocalBlockDetail::None, 1.0f / 60.0f))
        return fail(name, "changed or clear probe did not restart confirmation");
    NavAgentComponent crowd_nav;
    SteeringComponent crowd_steer;
    setPath(crowd_nav, {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, 0.0f)}, glm::vec3(8.0f, 0.0f, 0.0f));
    for (int i = 0; i < 44; ++i)
        if (tickNavigationRecovery(crowd_nav, crowd_steer, frozen,
                                   LocalBlockDetail::Crowd, 1.0f / 60.0f).category != NavFailureCategory::None)
            return fail(name, "crowd branch recovered early");
    const NavigationRecoveryResult crowd = tickNavigationRecovery(
        crowd_nav, crowd_steer, frozen, LocalBlockDetail::Crowd, 1.0f / 60.0f);
    if (crowd.category != NavFailureCategory::LocalBlock ||
        crowd.detail != LocalBlockDetail::Crowd || crowd_nav.recovery_count != 1)
        return fail(name, "continuous crowd probe did not recover as LocalBlock");

    // With no confirmed probe, the same frozen 45-tick steering window is a
    // distinct Stuck episode.
    NavAgentComponent stuck_nav;
    SteeringComponent stuck_steer;
    setPath(stuck_nav, {glm::vec3(0.0f), glm::vec3(8.0f, 0.0f, 0.0f)}, glm::vec3(8.0f, 0.0f, 0.0f));
    for (int i = 0; i < 44; ++i)
        if (tickNavigationRecovery(stuck_nav, stuck_steer, frozen, LocalBlockDetail::None,
                                   1.0f / 60.0f).category != NavFailureCategory::None)
            return fail(name, "unblocked frozen bot recovered early");
    const NavigationRecoveryResult stuck = tickNavigationRecovery(
        stuck_nav, stuck_steer, frozen, LocalBlockDetail::None, 1.0f / 60.0f);
    if (stuck.category != NavFailureCategory::Stuck ||
        stuck_nav.last_failure != NavFailureCategory::Stuck ||
        stuck_nav.recovery_count != 1 || stuck.detail != LocalBlockDetail::None)
        return fail(name, "unblocked frozen bot did not produce only Stuck");

    NavAgentComponent range_nav;
    range_nav.max_speed = 5.0f;
    range_nav.repath_cooldown = 0.25f;
    if (!botApprox(localBlockProbeRange(0.3f, range_nav), 1.25f) ||
        !botApprox(localBlockProbeRange(1.0f, range_nav), 2.05f))
        return fail(name, "probe range did not use capsule and backoff bounds");
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

bool testBotLocalSeparationDoorwayValues()
{
    const std::string name = "Bot_LocalSeparationDoorwayValues";
    using namespace Bots;
    const glm::vec3 forward(0.0f, 0.0f, 5.0f);

    // Empty and clearance-boundary rows preserve the authored route command.
    if (planLocalAvoidance(forward, {}, 5.0f).desired_velocity != forward ||
        planLocalAvoidance(forward, {{{kBotAvoidanceClearance, 0.0f, 0.0f}, false}}, 5.0f).desired_velocity != forward)
        return fail(name, "empty or clearance-boundary sample altered route steering");

    // Equal lateral samples cannot invent a left/right bias. An asymmetric
    // sample may turn away, but its horizontal correction and final command
    // stay inside the fixed caps.
    const LocalAvoidancePlan symmetric = planLocalAvoidance(forward,
        {{{0.45f, 0.0f, 0.0f}, false}, {{-0.45f, 0.0f, 0.0f}, false}}, 5.0f);
    if (!botApprox(symmetric.contribution.x, 0.0f) || !botApprox(symmetric.desired_velocity.x, 0.0f))
        return fail(name, "symmetric local samples caused lateral drift");
    const LocalAvoidancePlan asymmetric = planLocalAvoidance(forward,
        {{{0.20f, 0.0f, 0.35f}, false}, {{0.40f, 0.0f, -0.10f}, false}}, 5.0f);
    if (!std::isfinite(asymmetric.desired_velocity.x) || !std::isfinite(asymmetric.desired_velocity.z) ||
        asymmetric.desired_velocity.y != 0.0f ||
        glm::length(asymmetric.contribution) > kBotAvoidanceMaxContribution + 0.0001f ||
        glm::length(asymmetric.desired_velocity) > 5.0001f)
        return fail(name, "asymmetric local plan escaped finite horizontal caps");

    // A nine-body physics overlap can arrive in either incidental physics
    // order.  Stable actor ordering must select exactly the same eight bot
    // keys before the D13 steering cap is applied.
    std::vector<LocalActorCandidate> physics_order;
    for (const int32_t index : {7, 3, 9, 1, 8, 2, 6, 4, 5})
        physics_order.push_back({glm::vec3(static_cast<float>(index) * 0.1f, 0.0f, 0.0f), false,
                                 index, static_cast<uint16_t>(40000 + index)});
    const auto ordered = sortAndCapLocalActorCandidates(physics_order);
    std::reverse(physics_order.begin(), physics_order.end());
    const auto reversed = sortAndCapLocalActorCandidates(physics_order);
    if (ordered.size() != kBotAvoidanceSampleCap || reversed.size() != ordered.size())
        return fail(name, "stable candidate cap changed");
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (ordered[i].bot_index != static_cast<int32_t>(i + 1) ||
            ordered[i].bot_index != reversed[i].bot_index ||
            ordered[i].owner_client_id != reversed[i].owner_client_id)
            return fail(name, "nine-candidate selection depended on physics order");
    }
    const auto samples = makeLocalAvoidanceSamples(ordered, 5, 40005);
    if (!planLocalAvoidance(forward, samples, 5.0f).lower_doorway_priority)
        return fail(name, "lower stable bot key did not retain doorway priority");
    const auto human_samples = makeLocalAvoidanceSamples(
        {{glm::vec3(0.4f, 0.0f, 0.0f), true, 0, 7}}, 0, 40000);
    if (!planLocalAvoidance(forward, human_samples, 5.0f).lower_doorway_priority)
        return fail(name, "present human did not retain doorway priority");

    // Two, five, and nine actor doorway cohorts use the exact 15/45 fixed
    // tick wait. The lowest key moves; each other bot yields without letting
    // the normal AI-N1 Stuck branch create a second recovery.
    for (const int32_t cohort_size : {2, 5, 9}) {
        for (int32_t bot_index = 0; bot_index < cohort_size; ++bot_index) {
            NavAgentComponent nav;
            SteeringComponent steering;
            setPath(nav, {glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 8.0f)}, glm::vec3(0.0f, 0.0f, 8.0f));
            bool saw_yield = false;
            for (uint16_t tick = 1; tick < kBotLocalBlockConfirmTicks; ++tick) {
                const bool lower_priority = bot_index != 0;
                const bool yield = advanceDoorwayWait(nav, true, true, lower_priority);
                const NavigationRecoveryResult result = tickNavigationRecovery(
                    nav, steering, glm::vec3(0.0f), LocalBlockDetail::Crowd,
                    1.0f / 60.0f, yield);
                if (result.category != NavFailureCategory::None)
                    return fail(name, "doorway cohort recovered before the 45th tick");
                if (yield) saw_yield = true;
                if (yield && nav.stuck)
                    return fail(name, "deliberate doorway yield entered AI-N1 Stuck");
            }
            if ((bot_index == 0 && saw_yield) || (bot_index != 0 && !saw_yield))
                return fail(name, "doorway winner/yielder priority changed in cohort");
            const bool yield_at_boundary = advanceDoorwayWait(nav, true, true, bot_index != 0);
            const NavigationRecoveryResult boundary = tickNavigationRecovery(
                nav, steering, glm::vec3(0.0f), LocalBlockDetail::Crowd,
                1.0f / 60.0f, yield_at_boundary);
            if (boundary.category != NavFailureCategory::LocalBlock ||
                boundary.detail != LocalBlockDetail::Crowd || nav.recovery_count != 1 ||
                yield_at_boundary)
                return fail(name, "doorway boundary did not perform exactly one normal crowd recovery");
        }
    }

    NavAgentComponent bypass_nav;
    setPath(bypass_nav, {glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 8.0f)}, glm::vec3(0.0f, 0.0f, 8.0f));
    for (uint16_t tick = 0; tick < kBotDoorwayPriorityTicks; ++tick)
        (void)advanceDoorwayWait(bypass_nav, true, true, true);
    if (!bypass_nav.doorway_yielding || advanceDoorwayWait(bypass_nav, true, false, true) ||
        bypass_nav.doorway_crowd_ticks != 0 || bypass_nav.doorway_yielding)
        return fail(name, "combat/use bypass did not clear doorway state");
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
    auto requestDef = [&](const BuyPlanRequest& request) -> const WeaponCatalogDef* {
        const WeaponCatalogDef* def = findCatalogById(request.item_id);
        if (!def) return nullptr;
        return def;
    };
    auto validatePlan = [&](const BuyPlan& plan, FpsTeam team, int32_t budget) -> bool {
        std::array<size_t, static_cast<size_t>(BuyPlanGroup::Count)> group_counts{};
        std::array<int32_t, static_cast<size_t>(BuyPlanGroup::Count)> last_price{};
        std::array<bool, static_cast<size_t>(BuyPlanGroup::Count)> seen{};
        for (const BuyPlanRequest& request : plan.requests) {
            const size_t group = static_cast<size_t>(request.group);
            const WeaponCatalogDef* def = requestDef(request);
            if (group >= group_counts.size() || !def || !catalogAvailableToBot(*def, team) ||
                def->price > budget || ++group_counts[group] > kBotBuyAttemptsPerGroup)
                return false;
            if (seen[group] && def->price > last_price[group]) return false;
            seen[group] = true;
            last_price[group] = def->price;
            for (const BuyPlanRequest& prior : plan.requests)
                if (&prior != &request && prior.item_id == request.item_id)
                    return false;
        }
        return true;
    };
    auto firstInGroup = [](const BuyPlan& plan, BuyPlanGroup group) -> const BuyPlanRequest* {
        for (const BuyPlanRequest& request : plan.requests)
            if (request.group == group) return &request;
        return nullptr;
    };

    auto full = selectBuyPlan(5000, FpsTeam::Attackers, 1, 0);
    if (full.kind != BuyPlanKind::Full || !validatePlan(full, FpsTeam::Attackers, 5000))
        return fail(name, "attacker full plan was not a bounded catalog plan");
    const BuyPlanRequest* full_primary = firstInGroup(full, BuyPlanGroup::Primary);
    const BuyPlanRequest* full_armor = firstInGroup(full, BuyPlanGroup::Armor);
    if (!full_primary || !full_armor || requestDef(*full_primary)->slot != WeaponSlot::Primary ||
        requestDef(*full_primary)->price + requestDef(*full_armor)->price > 5000)
        return fail(name, "attacker full plan did not reserve a legal primary and armor");

    auto fullCt = selectBuyPlan(5000, FpsTeam::Defenders, 1, 0);
    const BuyPlanRequest* full_ct_primary = firstInGroup(fullCt, BuyPlanGroup::Primary);
    const BuyPlanRequest* full_ct_team_item = firstInGroup(fullCt, BuyPlanGroup::TeamEquipment);
    if (fullCt.kind != BuyPlanKind::Full || !validatePlan(fullCt, FpsTeam::Defenders, 5000) ||
        !full_ct_primary || !full_ct_team_item ||
        requestDef(*full_ct_primary)->slot != WeaponSlot::Primary ||
        requestDef(*full_ct_team_item)->kind != BuyItemKind::Equipment ||
        requestDef(*full_ct_team_item)->team_mask != kCatalogTeamDefenders)
        return fail(name, "defender full plan missed its legal team equipment");

    auto force = selectBuyPlan(2000, FpsTeam::Attackers, 1, 0);
    const BuyPlanRequest* force_primary = firstInGroup(force, BuyPlanGroup::Primary);
    const BuyPlanRequest* force_armor = firstInGroup(force, BuyPlanGroup::Armor);
    if (force.kind != BuyPlanKind::Force || !validatePlan(force, FpsTeam::Attackers, 2000) ||
        !force_primary || !force_armor ||
        requestDef(*force_primary)->price + requestDef(*force_armor)->price > 2000)
        return fail(name, "force plan exceeded its source-catalog budget");

    auto eco = selectBuyPlan(400, FpsTeam::Attackers, 1, 2);
    const BuyPlanRequest* eco_sidearm = firstInGroup(eco, BuyPlanGroup::Sidearm);
    if (eco.kind != BuyPlanKind::Eco || !validatePlan(eco, FpsTeam::Attackers, 400) || !eco_sidearm ||
        requestDef(*eco_sidearm)->slot != WeaponSlot::Secondary)
        return fail(name, "eco plan did not use an affordable catalog sidearm");

    auto pistol = selectBuyPlan(800, FpsTeam::Attackers, 1, 0);
    if (pistol.kind != BuyPlanKind::Pistol || !pistol.requests.empty())
        return fail(name, "normal pistol plan was not empty");
    auto pistolHard = selectBuyPlan(800, FpsTeam::Attackers, 2, 0);
    const BuyPlanRequest* pistol_armor = firstInGroup(pistolHard, BuyPlanGroup::Armor);
    if (pistolHard.kind != BuyPlanKind::Pistol || !validatePlan(pistolHard, FpsTeam::Attackers, 800) ||
        !pistol_armor || requestDef(*pistol_armor)->kind != BuyItemKind::Equipment)
        return fail(name, "hard pistol plan did not choose legal catalog equipment");

    // Invalid teams and budgets never yield an invalid request, and all
    // recoverable retry outcomes are explicit and bounded by per-group IDs.
    const BuyPlan no_team = selectBuyPlan(16000, FpsTeam::None, 3, 0);
    const BuyPlan no_money = selectBuyPlan(-1, FpsTeam::Attackers, 3, 0);
    if (no_team.kind != BuyPlanKind::Save || !no_team.requests.empty() ||
        no_money.kind != BuyPlanKind::Save || !no_money.requests.empty())
        return fail(name, "invalid team or wallet produced a buy request");
    if (!shouldTryNextBotBuyCandidate(BuyResult::UnknownItem) ||
        !shouldTryNextBotBuyCandidate(BuyResult::InsufficientFunds) ||
        !shouldTryNextBotBuyCandidate(BuyResult::WrongTeam) ||
        shouldTryNextBotBuyCandidate(BuyResult::DuplicateRequest) ||
        shouldTryNextBotBuyCandidate(BuyResult::PhaseClosed) ||
        shouldTryNextBotBuyCandidate(BuyResult::AlreadyOwned) ||
        shouldTryNextBotBuyCandidate(BuyResult::CarryLimit))
        return fail(name, "buy retry contract was not bounded to alternate candidates");

    // Cover every supported side/difficulty against small, exact-equipment,
    // force, full, and cap-sized wallets.  This deliberately asserts catalog
    // eligibility rather than a frozen alias choice.
    for (const FpsTeam supported_team : {FpsTeam::Attackers, FpsTeam::Defenders}) {
        for (uint8_t supported_difficulty = 0; supported_difficulty < 4; ++supported_difficulty) {
            for (const int32_t supported_budget : {0, 50, 200, 400, 650, 800, 1600, 2000, 3500, 5000, 16000}) {
                const BuyPlan generated = selectBuyPlan(supported_budget, supported_team,
                                                        supported_difficulty, 2);
                if (!validatePlan(generated, supported_team, supported_budget))
                    return fail(name, "catalog plan invalid for a supported side/difficulty/budget");
            }
        }
    }
    WeaponCatalogDef empty_catalog_row{};
    empty_catalog_row.price = 0;
    empty_catalog_row.team_mask = kCatalogTeamBoth;
    if (catalogAvailableToBot(empty_catalog_row, FpsTeam::Attackers))
        return fail(name, "empty catalog row became bot-buyable");
    return pass(name);
}

bool testBotAuthoredRouteData()
{
    const std::string name = "Bot_AuthoredRouteData";
    using namespace Bots;
    const std::string valid = R"({
        "format":"fpsshooter_bot_routes", "version":2, "level":"main",
        "anchors":[{"id":"a","site":0,"point":[0,0,0]},{"id":"b","site":1,"point":[20,0,0]}],
        "routes":[
            {"id":"t_a_long", "team":"attackers", "site":0, "phase":"live_unplanted", "role":"attack_approach", "points":[[0,0,0],[10,0,1]]},
            {"id":"t_b_short", "team":"attackers", "site":1, "phase":"live_unplanted", "role":"attack_approach", "points":[[0,0,0],[20,0,2]]}
        ],
        "holds":[
            {"id":"ct_a_hold", "team":"defenders", "site":0, "phase":"live_unplanted", "role":"hold", "point":[-5,0,3]},
            {"id":"ct_b_hold", "team":"defenders", "site":1, "phase":"live_unplanted", "role":"hold", "point":[-6,0,3]},
            {"id":"t_a_hold", "team":"attackers", "site":0, "phase":"live_planted", "role":"hold", "point":[10,0,1]},
            {"id":"t_b_hold", "team":"attackers", "site":1, "phase":"live_planted", "role":"hold", "point":[20,0,2]},
            {"id":"ct_a_retake", "team":"defenders", "site":0, "phase":"live_planted", "role":"retake", "point":[-5,0,3]},
            {"id":"ct_b_retake", "team":"defenders", "site":1, "phase":"live_planted", "role":"retake", "point":[-6,0,3]}
        ]
    })";
    AuthoredRouteData data;
    std::string error;
    if (!parseAuthoredRouteData(valid, data, &error))
        return fail(name, "valid route sidecar rejected: " + error);
    if (data.anchors.size() != 2 || data.routes.size() != 2 || data.holds.size() != 6 ||
        !validateAuthoredObjectiveData(data))
        return fail(name, "valid route collections parsed incorrectly");
    if (!authoredRouteLevelMatchesSelectedStem(data, "levels/main") ||
        authoredRouteLevelMatchesSelectedStem(data, "levels/de_block01"))
        return fail(name, "route sidecar declaration did not bind to its selected level stem");

    TeamObjectiveIntent attack{};
    if (!selectTeamObjective(data, Competitive::FpsTeam::Attackers, Competitive::RoundPhase::Live,
                             0, false, 0, attack) ||
        attack.kind != TeamObjectiveKind::AttackApproach || attack.site_index != 0)
        return fail(name, "public attacker objective was not selected");
    glm::vec3 target(0.0f);
    if (!selectBotObjectiveTarget(data, Competitive::FpsTeam::Attackers, attack, 9, target) ||
        !botApprox(target.x, 10.0f) || !botApprox(target.z, 1.0f))
        return fail(name, "attacker route target was not selected by canonical site");
    TeamObjectiveIntent defender_hold{};
    if (!selectTeamObjective(data, Competitive::FpsTeam::Defenders, Competitive::RoundPhase::Live,
                             0, false, 0, defender_hold) ||
        defender_hold.kind != TeamObjectiveKind::Hold ||
        !selectBotObjectiveTarget(data, Competitive::FpsTeam::Defenders, defender_hold, 1, target) ||
        !botApprox(target.x, -5.0f) || !botApprox(target.z, 3.0f))
        return fail(name, "defender unplanted hold target was not selected deterministically");
    TeamObjectiveIntent retake{};
    if (!selectTeamObjective(data, Competitive::FpsTeam::Defenders, Competitive::RoundPhase::Live,
                             0, true, 0, retake) ||
        retake.kind != TeamObjectiveKind::Retake ||
        !selectBotObjectiveTarget(data, Competitive::FpsTeam::Defenders, retake, 1, target) ||
        !botApprox(target.x, -5.0f) || !botApprox(target.z, 3.0f))
        return fail(name, "defender retake target was not selected deterministically");

    if (parseAuthoredRouteData(R"({"format":"fpsshooter_bot_routes","version":1,"level":"main","routes":[]})", data, &error) || !data.empty())
        return fail(name, "v1 tactical sidecar was accepted");
    if (parseAuthoredRouteData(R"({"format":"fpsshooter_bot_routes","version":2,"level":"main","anchors":[],"routes":[],"holds":[],"patrols":[]})", data, &error) || !data.empty())
        return fail(name, "removed patrol collection was accepted");
    if (parseAuthoredRouteData(R"({"format":"fpsshooter_bot_routes","version":2,"level":"main","anchors":[{"id":"a","site":0,"point":[0,0,0]},{"id":"a","site":1,"point":[0,0,0]}],"routes":[],"holds":[]})", data, &error) || !data.empty())
        return fail(name, "duplicate authored id was accepted");
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
    BotDumpInput d; d.bot_index = 2; d.team = 1; d.state = BotState::Advance; d.hp = 77;
    d.route_active = true; d.path_valid = true; d.stuck = false; d.visible_age = 0.5f;
    d.heard_age = 2.0f; d.heard_category = BotHeardNoiseCategory::WeaponFire;
    std::string s = formatBotDump(d);
    for (const char* k : {"[BotDebug]", "bot=2", "team=1", "state=Advance", "hp=77", "route_active=1", "path_valid=1", "stuck=0", "percept_visible_age=", "percept_heard_category=WeaponFire", "percept_heard_age="}) if (s.find(k) == std::string::npos) return fail(name, std::string("missing key: ") + k);
    for (const char* forbidden : {"pos=", "position=", "goal=", "1.00,2.00,3.00", "4.00,0.00,5.00"})
        if (s.find(forbidden) != std::string::npos)
            return fail(name, std::string("coordinate-bearing debug field leaked: ") + forbidden);
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
