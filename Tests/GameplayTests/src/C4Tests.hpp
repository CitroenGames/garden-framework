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
