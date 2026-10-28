#pragma once
// C4 Stage 3.4 tests included by GameplayTests main.cpp (uses its fail/pass helpers).
#include "shared/CompetitiveLogic.hpp"
#include "shared/BombSiteTriggers.hpp"
#include <cmath>
#include <string>

namespace {
Competitive::BombSystem makeBombSys(entt::entity carrier)
{
    Competitive::BombSystem b;
    b.config.sites = {Competitive::BombSite{glm::vec3(0, 0, 0), 5.0f, 0}};
    b.assignCarrier(carrier);
    return b;
}
Competitive::BombTickInput plantInput()
{
    Competitive::BombTickInput in; in.plant_use = true; in.plant_in_site = true; in.plant_site = 0;
    in.carrier_position = glm::vec3(0, 0, 0); return in;
}
void plantFully(Competitive::BombSystem& b)
{
    auto in = plantInput();
    for (int i = 0; i < 200 && b.bomb.state != Competitive::BombState::Planted; ++i) b.tick(1.0f / 60.0f, in);
}
Competitive::BombTickInput defuseInput(entt::entity actor, bool kit)
{
    Competitive::BombTickInput in; in.defuse_actor = actor; in.defuse_use = true; in.defuse_in_range = true; in.defuse_has_kit = kit; return in;
}
} // namespace

bool testC4PlantInterrupt()
{
    const std::string name = "C4_PlantInterrupt";
    using namespace Competitive;
    entt::registry reg; entt::entity carrier = reg.create();
    { BombSystem b = makeBombSys(carrier); auto in = plantInput();
      for (int i = 0; i < 179; ++i) b.tick(1.0f / 60.0f, in);
      if (b.bomb.state == BombState::Planted) return fail(name, "planted before 3.0s");
      b.tick(1.0f / 60.0f, in);
      if (b.bomb.state != BombState::Planted) return fail(name, "3.0s plant in site did not complete");
      if (std::abs(b.bomb.timer - 40.0f) > 0.001f) return fail(name, "timer not 40 on plant"); }
    { BombSystem b = makeBombSys(carrier); auto in = plantInput(); in.plant_in_site = false;
      for (int i = 0; i < 300; ++i) b.tick(1.0f / 60.0f, in);
      if (b.bomb.state != BombState::Carried || b.bomb.plant_channel != 0.0f) return fail(name, "plant outside site progressed"); }
    for (int kind = 0; kind < 4; ++kind) {
        BombSystem b = makeBombSys(carrier); auto in = plantInput();
        for (int i = 0; i < 90; ++i) b.tick(1.0f / 60.0f, in);
        if (b.bomb.state != BombState::Planting) return fail(name, "not planting at 1.5s kind=" + std::to_string(kind));
        auto bad = in;
        if (kind == 0) bad.plant_moved = true; else if (kind == 1) bad.plant_damaged = true;
        else if (kind == 2) bad.plant_weapon_switched = true; else bad.plant_dead = true;
        b.tick(1.0f / 60.0f, bad);
        if (kind == 3) { if (b.bomb.state != BombState::Dropped) return fail(name, "death did not drop bomb"); }
        else if (b.bomb.state != BombState::Carried || b.bomb.plant_channel != 0.0f) return fail(name, "interrupt kind=" + std::to_string(kind) + " did not reset channel");
        if (kind != 3) { for (int i = 0; i < 179; ++i) b.tick(1.0f / 60.0f, in);
          if (b.bomb.state == BombState::Planted) return fail(name, "restarted plant finished early kind=" + std::to_string(kind)); }
    }
    { BombSystem b = makeBombSys(carrier); auto in = plantInput();
      for (int i = 0; i < 60; ++i) b.tick(1.0f / 60.0f, in);
      auto out = in; out.plant_in_site = false; b.tick(1.0f / 60.0f, out);
      if (b.bomb.state != BombState::Carried || b.bomb.plant_channel != 0.0f) return fail(name, "leaving site did not cancel"); }
    { BombSystem b = makeBombSys(carrier); auto in = plantInput();
      for (int i = 0; i < 60; ++i) b.tick(1.0f / 60.0f, in);
      auto rel = in; rel.plant_use = false; b.tick(1.0f / 60.0f, rel);
      if (b.bomb.state != BombState::Carried) return fail(name, "USE release did not cancel"); }
    return pass(name);
}

bool testC4AuthoredTriggerOccupancy()
{
    const std::string name = "C4_AuthoredTriggerOccupancy";
    using namespace Competitive;
    entt::registry reg;
    const entt::entity carrier = reg.create();
    const entt::entity site_a = reg.create();
    reg.emplace<BombSiteComponent>(site_a, BombSiteComponent{1});
    reg.emplace<TriggerComponent>(site_a, TriggerComponent{true, 1, true});
    auto& state = reg.emplace<TriggerOverlapState>(site_a);

    BombSystem bomb;
    if (!bomb.assignCarrier(carrier)) return fail(name, "carrier assignment failed");
    auto tickFromOccupancy = [&](bool use) {
        BombTickInput in;
        in.plant_use = use;
        in.carrier_position = glm::vec3(0.0f);
        in.plant_in_site = findBombSiteForOccupant(reg, carrier, in.plant_site);
        bomb.tick(1.0f / 60.0f, in);
    };

    for (int i = 0; i < 180; ++i) tickFromOccupancy(true);
    if (bomb.bomb.state != BombState::Carried || bomb.bomb.plant_channel != 0.0f)
        return fail(name, "carrier outside authored trigger advanced planting");

    state.occupants.push_back(carrier);
    for (int i = 0; i < 60; ++i) tickFromOccupancy(true);
    if (bomb.bomb.state != BombState::Planting || bomb.bomb.plant_channel <= 0.0f)
        return fail(name, "occupancy did not start plant channel");

    state.occupants.clear();
    tickFromOccupancy(true);
    if (bomb.bomb.state != BombState::Carried || bomb.bomb.plant_channel != 0.0f)
        return fail(name, "leaving authored trigger did not cancel channel");

    state.occupants.push_back(carrier);
    for (int i = 0; i < 180; ++i) tickFromOccupancy(true);
    if (bomb.bomb.state != BombState::Planted || bomb.bomb.site != 1)
        return fail(name, "continuous authored-trigger occupancy did not complete plant");
    return pass(name);
}

bool testC4DefuseKit()
{
    const std::string name = "C4_DefuseKit";
    using namespace Competitive;
    entt::registry reg; entt::entity carrier = reg.create(); entt::entity ct = reg.create(); entt::entity ct2 = reg.create();
    auto runCase = [&](bool kit, float expect_seconds) -> bool {
        BombSystem b = makeBombSys(carrier); plantFully(b);
        if (b.bomb.state != BombState::Planted) return false;
        auto in = defuseInput(ct, kit);
        int ticks = 0; const int expect_ticks = (int)std::lround(expect_seconds * 60.0f);
        while (b.bomb.state != BombState::Defused && ticks < expect_ticks + 5) { b.tick(1.0f / 60.0f, in); ++ticks; }
        if (b.bomb.state != BombState::Defused) return false;
        if (std::abs(ticks - expect_ticks) > 1) return false;
        entt::entity who = entt::null; if (!b.consumeDefuseCompleted(who) || who != ct) return false;
        if (!b.consumeDefused()) return false;
        return true;
    };
    if (!runCase(true, 5.0f)) return fail(name, "kit defuse not 5.0s +/-1 tick");
    if (!runCase(false, 10.0f)) return fail(name, "no-kit defuse not 10.0s +/-1 tick");
    { BombSystem b = makeBombSys(carrier); plantFully(b);
      auto first = defuseInput(ct, false);
      for (int i = 0; i < 60; ++i) b.tick(1.0f / 60.0f, first);
      if (b.bomb.state != BombState::Defusing || b.bomb.defuser != ct) return fail(name, "first defuser not locked");
      float ch = b.bomb.defuse_channel;
      auto second = defuseInput(ct2, true);
      for (int i = 0; i < 60; ++i) b.tick(1.0f / 60.0f, second);
      if (b.bomb.defuser != ct || b.bomb.defuse_channel != ch) return fail(name, "second defuser stole/reset channel");
      auto dead = first; dead.defuse_dead = true; b.tick(1.0f / 60.0f, dead);
      if (b.bomb.state != BombState::Planted || b.bomb.defuse_channel != 0.0f) return fail(name, "defuser death did not cancel"); }
    return pass(name);
}

bool testC4Timer40()
{
    const std::string name = "C4_Timer40";
    using namespace Competitive;
    entt::registry reg; entt::entity carrier = reg.create(); entt::entity ct = reg.create();
    { BombSystem b = makeBombSys(carrier); plantFully(b);
      BombTickInput idle; int ticks = 0;
      while (b.bomb.state != BombState::Exploded && ticks < 40 * 60 + 5) { b.tick(1.0f / 60.0f, idle); ++ticks; }
      if (b.bomb.state != BombState::Exploded) return fail(name, "no detonation at 40s");
      if (std::abs(ticks - 40 * 60) > 1) return fail(name, "detonation tick off: " + std::to_string(ticks));
      if (!b.consumeDetonated() || b.consumeDetonated()) return fail(name, "detonated consume not exactly-once"); }
    { BombSystem b = makeBombSys(carrier); plantFully(b);
      BombTickInput idle; for (int i = 0; i < 30 * 60; ++i) b.tick(1.0f / 60.0f, idle);
      auto in = defuseInput(ct, false);
      for (int i = 0; i < 12 * 60 && b.bomb.state != BombState::Exploded && b.bomb.state != BombState::Defused; ++i) b.tick(1.0f / 60.0f, in);
      if (b.bomb.state != BombState::Exploded) return fail(name, "tie did not explode");
      if (b.consumeDefused()) return fail(name, "tie reported defused"); }
    { BombSystem b = makeBombSys(carrier); BombHooksAdapter hooks(&b);
      RoundSystem rs; rs.configure(fastCompConfig()); rs.bomb_hooks = &hooks;
      tickComp(rs, 0.01f, bothAlive());
      for (int i = 0; i < 20 && rs.state.phase != RoundPhase::Live; ++i) tickComp(rs, 0.06f, bothAlive());
      plantFully(b);
      if (!hooks.isPlanted()) return fail(name, "hooks isPlanted false after plant");
      entt::entity planter = entt::null; if (!b.consumePlantCompleted(planter) || planter != carrier) return fail(name, "plant-completed hook wrong");
      BombTickInput idle; for (int i = 0; i < 40 * 60 + 2; ++i) b.tick(1.0f / 60.0f, idle);
      tickComp(rs, 0.01f, bothAlive());
      if (rs.state.phase != RoundPhase::RoundEnd || rs.state.last_reason != RoundEndReason::TargetBombed) return fail(name, "RoundSystem did not end TargetBombed"); }
    return pass(name);
}

bool testC4CarrierDropPickup()
{
    const std::string name = "C4_CarrierDropPickup";
    using namespace Competitive;
    entt::registry reg; entt::entity carrier = reg.create(); entt::entity atk2 = reg.create(); entt::entity def = reg.create();
    BombSystem b = makeBombSys(carrier);
    if (!b.onCarrierDeath(glm::vec3(2, 0, 0))) return fail(name, "carrier death did not drop");
    if (b.bomb.state != BombState::Dropped) return fail(name, "not Dropped after death");
    if (b.tryPickup(def, FpsTeam::Defenders, glm::vec3(2, 0, 0))) return fail(name, "Defender picked up bomb");
    if (b.tryPickup(atk2, FpsTeam::Attackers, glm::vec3(50, 0, 0))) return fail(name, "far Attacker picked up bomb");
    if (!b.tryPickup(atk2, FpsTeam::Attackers, glm::vec3(2, 0, 0))) return fail(name, "Attacker pickup failed");
    if (b.bomb.state != BombState::Carried || b.bomb.carrier != atk2) return fail(name, "pickup carrier wrong");
    if (!b.onCarrierDisconnect(glm::vec3(3, 0, 0))) return fail(name, "disconnect did not drop");
    if (b.bomb.state != BombState::Dropped) return fail(name, "not Dropped after disconnect");
    BombSystem b2 = makeBombSys(carrier); auto in = plantInput(); in.paused = true;
    for (int i = 0; i < 300; ++i) b2.tick(1.0f / 60.0f, in);
    if (b2.bomb.state != BombState::Carried) return fail(name, "paused plant progressed");
    return pass(name);
}

bool testC4WorldAuthority()
{
    const std::string name = "C4_WorldAuthority";
    using namespace Competitive;

    entt::registry first_world;
    entt::registry second_world;
    const entt::entity first_carrier = first_world.create();
    const entt::entity second_carrier = second_world.create();
    auto& first = first_world.ctx().emplace<CompetitiveWorldContext>();
    auto& second = second_world.ctx().emplace<CompetitiveWorldContext>();
    first.enabled = true;
    second.enabled = true;
    first.bomb.config.sites = {BombSite{glm::vec3(0.0f), 5.0f, 0}};
    second.bomb.config.sites = {BombSite{glm::vec3(100.0f), 5.0f, 1}};
    first.bomb.config.plant_seconds = 0.05f;
    first.bomb.config.bomb_timer_seconds = 0.10f;
    if (!first.bomb.assignCarrier(first_carrier) || !second.bomb.assignCarrier(second_carrier))
        return fail(name, "independent worlds did not receive separate carriers");

    // Channel cancellation stays local to its owning world.
    auto plant = plantInput();
    for (int i = 0; i < 3; ++i) first.bomb.tick(0.01f, plant);
    if (first.bomb.bomb.state != BombState::Planting || second.bomb.bomb.state != BombState::Carried)
        return fail(name, "plant channel crossed world boundary");
    auto interrupted = plant;
    interrupted.plant_damaged = true;
    first.bomb.tick(0.01f, interrupted);
    if (first.bomb.bomb.state != BombState::Carried || first.bomb.bomb.plant_channel != 0.0f)
        return fail(name, "damage interrupt did not reset the local channel");
    if (second.bomb.bomb.state != BombState::Carried || second.bomb.bomb.plant_channel != 0.0f)
        return fail(name, "damage interrupt mutated the other world");

    first.round.configure(fastCompConfig());
    first.round.bomb_hooks = &first.bomb_hooks;
    tickComp(first.round, 0.01f, bothAlive());
    for (int i = 0; i < 20 && first.round.state.phase != RoundPhase::Live; ++i)
        tickComp(first.round, 0.06f, bothAlive());
    for (int i = 0; i < 8 && first.bomb.bomb.state != BombState::Planted; ++i)
        first.bomb.tick(0.01f, plant);
    if (first.bomb.bomb.state != BombState::Planted)
        return fail(name, "local bomb did not plant after a restarted channel");
    if (second.bomb.bomb.state != BombState::Carried)
        return fail(name, "plant completion leaked into the other world");

    // The timer boundary is owned by this bomb, and the existing hook still
    // supplies the one round outcome after the detonation is consumed.
    BombTickInput idle;
    first.bomb.tick(0.04f, idle);
    if (first.bomb.bomb.state != BombState::Planted)
        return fail(name, "bomb expired before its timer boundary");
    first.bomb.tick(0.06f, idle);
    if (first.bomb.bomb.state != BombState::Exploded || second.bomb.bomb.state != BombState::Carried)
        return fail(name, "timer boundary was not isolated");
    tickComp(first.round, 0.01f, bothAlive());
    if (first.round.state.phase != RoundPhase::RoundEnd || first.round.state.last_reason != RoundEndReason::TargetBombed)
        return fail(name, "world-owned bomb hook did not end the round");

    first_world.ctx().erase<CompetitiveWorldContext>();
    if (first_world.ctx().find<CompetitiveWorldContext>())
        return fail(name, "world context survived teardown");
    auto& restarted = first_world.ctx().emplace<CompetitiveWorldContext>();
    if (restarted.enabled || restarted.bomb.bomb.state != BombState::Idle || restarted.round_heartbeat != 0.0f)
        return fail(name, "recreated world inherited match authority");
    return pass(name);
}

bool testC4ObjectiveInterruptionsAndTeardown()
{
    const std::string name = "C4_ObjectiveInterruptionsAndTeardown";
    using namespace Competitive;
    entt::registry reg;
    const entt::entity carrier = reg.create();
    const entt::entity defuser = reg.create();
    const entt::entity second_defuser = reg.create();

    // Pausing freezes a live plant channel instead of resetting or advancing
    // it. The caller owns whether a match is paused; the bomb owns no second
    // pause clock.
    {
        BombSystem b = makeBombSys(carrier);
        auto plant = plantInput();
        for (int i = 0; i < 30; ++i) b.tick(1.0f / 60.0f, plant);
        const float held = b.bomb.plant_channel;
        auto paused = plant;
        paused.paused = true;
        for (int i = 0; i < 120; ++i) b.tick(1.0f / 60.0f, paused);
        if (b.bomb.state != BombState::Planting || std::abs(b.bomb.plant_channel - held) > 0.0001f)
            return fail(name, "pause changed an active plant channel");
    }

    auto startDefuse = [&]() {
        BombSystem b = makeBombSys(carrier);
        plantFully(b);
        auto input = defuseInput(defuser, false);
        for (int i = 0; i < 30; ++i) b.tick(1.0f / 60.0f, input);
        return b;
    };

    // Starting a channel has the same stability rule as maintaining one.
    // Residual movement or damage on an otherwise valid USE sample must not
    // create a start/cancel transition in the same tick.
    {
        BombSystem b = makeBombSys(carrier);
        plantFully(b);
        int defuse_starts = 0;
        b.on_transition = [&defuse_starts](BombState from, BombState to) {
            if (from == BombState::Planted && to == BombState::Defusing)
                ++defuse_starts;
        };
        auto moving = defuseInput(defuser, false);
        moving.defuse_moved = true;
        b.tick(1.0f / 60.0f, moving);
        auto damaged = defuseInput(defuser, false);
        damaged.defuse_damaged = true;
        b.tick(1.0f / 60.0f, damaged);
        if (b.bomb.state != BombState::Planted || b.bomb.defuser != entt::null ||
            b.bomb.defuse_channel != 0.0f || defuse_starts != 0)
            return fail(name, "unstable USE started a defuse channel");
        const auto stable = defuseInput(defuser, false);
        b.tick(1.0f / 60.0f, stable);
        if (b.bomb.state != BombState::Defusing || b.bomb.defuser != defuser ||
            defuse_starts != 1)
            return fail(name, "stable USE did not start exactly one defuse channel");
        b.tick(1.0f / 60.0f, stable);
        if (defuse_starts != 1)
            return fail(name, "active stable channel reported a second start");
    }

    // Damage, movement, leaving range, weapon/USE loss (the supported input
    // proxy), death, and explicit disconnect teardown all clear the channel.
    for (int kind = 0; kind < 6; ++kind) {
        BombSystem b = startDefuse();
        if (b.bomb.state != BombState::Defusing || b.bomb.defuser != defuser)
            return fail(name, "defuse did not start kind=" + std::to_string(kind));
        auto interrupted = defuseInput(defuser, false);
        if (kind == 0) interrupted.defuse_damaged = true;
        else if (kind == 1) interrupted.defuse_moved = true;
        else if (kind == 2) interrupted.defuse_in_range = false;
        else if (kind == 3) interrupted.defuse_use = false;
        else if (kind == 4) interrupted.defuse_dead = true;
        else { interrupted.defuse_actor = entt::null; interrupted.defuse_interrupted = true; }
        b.tick(1.0f / 60.0f, interrupted);
        if (b.bomb.state != BombState::Planted || b.bomb.defuser != entt::null || b.bomb.defuse_channel != 0.0f)
            return fail(name, "defuse interruption did not reset kind=" + std::to_string(kind));
    }

    // A second defender cannot replace a still-valid defuser. Their input is
    // ignored until the channel becomes explicitly invalid.
    {
        BombSystem b = startDefuse();
        const float held = b.bomb.defuse_channel;
        b.tick(1.0f / 60.0f, defuseInput(second_defuser, true));
        if (b.bomb.state != BombState::Defusing || b.bomb.defuser != defuser ||
            std::abs(b.bomb.defuse_channel - held) > 0.0001f)
            return fail(name, "second defuser replaced active channel");
    }

    // The exact final timer tick wins before defuse progress, preserving the
    // state machine's documented deterministic explosion tie policy.
    {
        BombSystem b = startDefuse();
        b.bomb.timer = 1.0f / 60.0f;
        b.tick(1.0f / 60.0f, defuseInput(defuser, true));
        if (b.bomb.state != BombState::Exploded || !b.consumeDetonated() || b.consumeDefused())
            return fail(name, "exact timer boundary did not detonate once");
    }

    // Competitive teardown removes the per-world bomb authority and its
    // mode-owned player state so a reused registry starts a new objective.
    {
        entt::registry world;
        const entt::entity player = world.create();
        auto& context = startCompetitiveWorldRuntime(world, ModeConfig{});
        if (!context.bomb.assignCarrier(player))
            return fail(name, "world context did not own carrier");
        world.emplace<MoneyComponent>(player);
        world.emplace<ArmorComponent>(player);
        stopCompetitiveWorldRuntime(world);
        if (world.ctx().find<CompetitiveWorldContext>() ||
            world.all_of<MoneyComponent, ArmorComponent>(player))
            return fail(name, "world teardown retained objective authority");
    }
    return pass(name);
}
