#include "Gameplay/GameplayComponents.hpp"
#include "Gameplay/GameplaySystems.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Scheduler/SystemScheduler.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
bool fail(const std::string& n, const std::string& r)
{
    std::cerr << "[FAIL] " << n << ": " << r << std::endl;
    return false;
}
bool pass(const std::string& n)
{
    std::cout << "[PASS] " << n << std::endl;
    return true;
}
bool approx(float a, float b, float eps = 0.001f) { return std::abs(a - b) <= eps; }

int g_died = 0;
int g_applied = 0;
int g_timer_expired = 0;
int g_chan_completed = 0;
int g_chan_cancelled = 0;
float g_last_remaining = -1.0f;
bool g_last_killed = false;
void onDied(const EntityDiedEvent&) { ++g_died; }
void onApplied(const DamageAppliedEvent& e) { ++g_applied; g_last_remaining = e.remaining_health; g_last_killed = e.killed; }
void onTimer(const TimerExpiredEvent&) { ++g_timer_expired; }
void onChanDone(const ChannelCompletedEvent&) { ++g_chan_completed; }
void onChanCancel(const ChannelCancelledEvent&) { ++g_chan_cancelled; }

void connectAll()
{
    EventBus::get().sink<EntityDiedEvent>().connect<&onDied>();
    EventBus::get().sink<DamageAppliedEvent>().connect<&onApplied>();
    EventBus::get().sink<TimerExpiredEvent>().connect<&onTimer>();
    EventBus::get().sink<ChannelCompletedEvent>().connect<&onChanDone>();
    EventBus::get().sink<ChannelCancelledEvent>().connect<&onChanCancel>();
}
void disconnectAll()
{
    EventBus::get().sink<EntityDiedEvent>().disconnect<&onDied>();
    EventBus::get().sink<DamageAppliedEvent>().disconnect<&onApplied>();
    EventBus::get().sink<TimerExpiredEvent>().disconnect<&onTimer>();
    EventBus::get().sink<ChannelCompletedEvent>().disconnect<&onChanDone>();
    EventBus::get().sink<ChannelCancelledEvent>().disconnect<&onChanCancel>();
    EventBus::get().clear();
}

bool testWallet()
{
    const std::string name = "wallet spend-fail no-change and cap clamp";
    ResourceWalletComponent w;
    w.balance = 50;
    w.cap = 100;
    if (w.trySpend(60)) return fail(name, "overspend succeeded");
    if (w.balance != 50) return fail(name, "balance changed on failed spend");
    if (w.trySpend(-5)) return fail(name, "negative spend succeeded");
    if (w.balance != 50) return fail(name, "balance changed on negative spend");
    if (!w.trySpend(50)) return fail(name, "exact spend failed");
    if (w.balance != 0) return fail(name, "balance not 0 after exact spend");
    // cap clamp
    w.balance = 90;
    const int64_t added = w.add(50);
    if (added != 10 || w.balance != 100) return fail(name, "cap clamp failed");
    if (w.add(10) != 0 || w.balance != 100) return fail(name, "add at cap changed balance");
    if (w.add(-1) != 0) return fail(name, "negative add not ignored");
    w.setBalance(1000000);
    if (w.balance != 100) return fail(name, "setBalance not clamped to cap");
    if (!w.canAfford(100) || w.canAfford(101)) return fail(name, "canAfford wrong");
    return pass(name);
}

bool testInventory()
{
    const std::string name = "inventory add/select/drop-count validation";
    InventoryComponent inv;
    if (inv.active_slot != -1) return fail(name, "active_slot should start -1");
    if (!inv.add(1001, 3)) return fail(name, "add 1001 failed");
    if (!inv.add(1001, 2)) return fail(name, "stack add failed");
    if (inv.countOf(1001) != 5) return fail(name, "stack count != 5");
    if (!inv.add(2002, 1)) return fail(name, "add 2002 failed");
    if (inv.findSlot(2002) < 0) return fail(name, "2002 slot missing");
    const int32_t slot_2002 = inv.findSlot(2002);
    if (!inv.select(slot_2002)) return fail(name, "select occupied failed");
    if (inv.active_slot != slot_2002) return fail(name, "active_slot not set");
    if (inv.select(7)) return fail(name, "select empty slot succeeded");
    if (inv.active_slot != slot_2002) return fail(name, "active changed on failed select");
    if (inv.select(-1) || inv.select(99)) return fail(name, "select out-of-range succeeded");
    if (inv.add(0, 1)) return fail(name, "add def 0 succeeded");
    if (inv.add(3003, 0)) return fail(name, "add count 0 succeeded");
    // drop-count from slot
    const int32_t slot_1001 = inv.findSlot(1001);
    if (inv.removeFromSlot(slot_1001, 2) != 2) return fail(name, "drop 2 failed");
    if (inv.countOf(1001) != 3) return fail(name, "count after drop != 3");
    if (inv.removeFromSlot(slot_1001, 99) != 3) return fail(name, "drop over-count clamp failed");
    if (inv.countOf(1001) != 0) return fail(name, "slot not cleared after drop all");
    if (inv.getSlot(slot_1001)->empty() != true) return fail(name, "slot not empty");
    // remove across slots with no-mutation-on-fail
    InventoryComponent inv2;
    inv2.add(42, 2);
    if (inv2.remove(42, 5)) return fail(name, "remove insufficient succeeded");
    if (inv2.countOf(42) != 2) return fail(name, "failed remove mutated state");
    if (!inv2.remove(42, 2)) return fail(name, "remove exact failed");
    // active cleared when its slot emptied
    InventoryComponent inv3;
    inv3.add(7, 1);
    const int32_t s = inv3.findSlot(7);
    inv3.select(s);
    inv3.removeFromSlot(s, 1);
    if (inv3.active_slot != -1) return fail(name, "active not cleared on emptying active slot");
    return pass(name);
}

bool testDamage()
{
    const std::string name = "damage kill emits died once, overkill and invulnerable";
    EventBus::get().clear();
    g_died = 0; g_applied = 0; g_last_remaining = -1; g_last_killed = false;
    connectAll();
    entt::registry reg;
    auto target = reg.create();
    reg.emplace<HealthComponent>(target, HealthComponent{100.0f, 100.0f, true, 0});
    auto attacker = reg.create();
    reg.emplace<IdentityComponent>(attacker, generateEntityGuid());
    reg.emplace<IdentityComponent>(target, generateEntityGuid());
    DamageInfo info;
    info.amount = 30.0f;
    info.instigator = entityRefOf(reg, attacker);
    info.source = info.instigator;
    info.damage_type = 1;
    auto r1 = DamageSystem::applyDamage(reg, target, info, 10);
    if (!r1.applied || r1.killed || !approx(r1.remaining_health, 70.0f)) { disconnectAll(); return fail(name, "non-lethal damage wrong"); }
    if (g_applied != 1 || g_died != 0) { disconnectAll(); return fail(name, "events after non-lethal wrong"); }
    // overkill
    info.amount = 500.0f;
    auto r2 = DamageSystem::applyDamage(reg, target, info, 11);
    if (!r2.applied || !r2.killed || !approx(r2.remaining_health, 0.0f)) { disconnectAll(); return fail(name, "overkill result wrong"); }
    if (!approx(reg.get<HealthComponent>(target).current, 0.0f) || reg.get<HealthComponent>(target).alive) { disconnectAll(); return fail(name, "health state after kill wrong"); }
    if (g_died != 1 || g_applied != 2 || !g_last_killed) { disconnectAll(); return fail(name, "died not emitted exactly once on kill"); }
    // damage after death: blocked, no second died
    auto r3 = DamageSystem::applyDamage(reg, target, info, 12);
    if (r3.applied || !r3.blocked || g_died != 1 || g_applied != 2) { disconnectAll(); return fail(name, "post-death damage not blocked / died twice"); }
    // invulnerable blocks
    auto t2 = reg.create();
    reg.emplace<IdentityComponent>(t2, generateEntityGuid());
    auto& h2 = reg.emplace<HealthComponent>(t2, HealthComponent{80.0f, 80.0f, true, 0});
    h2.invulnerable_until_tick = 100;
    DamageInfo info2; info2.amount = 10.0f; info2.damage_type = 1;
    auto rb = DamageSystem::applyDamage(reg, t2, info2, 99);
    if (rb.applied || !rb.blocked || !approx(reg.get<HealthComponent>(t2).current, 80.0f)) { disconnectAll(); return fail(name, "invulnerable did not block at tick 99"); }
    auto ra = DamageSystem::applyDamage(reg, t2, info2, 100);
    if (!ra.applied || !approx(reg.get<HealthComponent>(t2).current, 70.0f)) { disconnectAll(); return fail(name, "damage at invulnerable_until_tick should apply"); }
    // zero/negative blocked, missing health blocked
    auto t3 = reg.create();
    DamageInfo zero; zero.amount = 0.0f;
    if (DamageSystem::applyDamage(reg, t3, zero, 0).applied) { disconnectAll(); return fail(name, "damage without HealthComponent applied"); }
    disconnectAll();
    return pass(name);
}

bool testTimerExactTick()
{
    const std::string name = "timer expiry exact tick";
    EventBus::get().clear();
    g_timer_expired = 0;
    connectAll();
    entt::registry reg;
    auto e = reg.create();
    auto& timer = reg.emplace<CountdownTimerComponent>(e);
    timer.start(0.05f, false); // 3 ticks at 60Hz
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    registerGameplaySystems(sched);
    if (!sched.hasSystem("engine.gameplay.countdown_timers")) { disconnectAll(); return fail(name, "countdown system not registered"); }
    auto order = sched.executionOrder(Phase::Gameplay);
    bool in_gameplay = false;
    for (auto& s : order) if (s == "engine.gameplay.countdown_timers") in_gameplay = true;
    if (!in_gameplay) { disconnectAll(); return fail(name, "countdown system not in Gameplay phase"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 0) { disconnectAll(); return fail(name, "expired on tick 1"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 0) { disconnectAll(); return fail(name, "expired on tick 2"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 1) { disconnectAll(); return fail(name, "did not expire on tick 3, count=" + std::to_string(g_timer_expired)); }
    if (reg.get<CountdownTimerComponent>(e).running) { disconnectAll(); return fail(name, "non-loop timer still running after expiry"); }
    if (!approx(reg.get<CountdownTimerComponent>(e).remaining, 0.0f)) { disconnectAll(); return fail(name, "remaining not 0 after expiry"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 1) { disconnectAll(); return fail(name, "non-loop timer expired twice"); }
    // loop timer expires repeatedly
    auto e2 = reg.create();
    auto& lt = reg.emplace<CountdownTimerComponent>(e2);
    lt.start(1.0f / 60.0f, true);
    g_timer_expired = 0;
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 1) { disconnectAll(); return fail(name, "loop timer did not expire tick 1"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().flush();
    if (g_timer_expired != 2) { disconnectAll(); return fail(name, "loop timer did not expire tick 2"); }
    if (!reg.get<CountdownTimerComponent>(e2).running) { disconnectAll(); return fail(name, "loop timer stopped"); }
    disconnectAll();
    return pass(name);
}

bool testChannel()
{
    const std::string name = "channel tick complete and cancel resets";
    EventBus::get().clear();
    g_chan_completed = 0; g_chan_cancelled = 0;
    connectAll();
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<IdentityComponent>(e, generateEntityGuid());
    auto& ch = reg.emplace<ChannelComponent>(e);
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    registerGameplaySystems(sched);
    if (!Gameplay::startChannel(reg, e, 42, 0.05f)) { disconnectAll(); return fail(name, "startChannel failed"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    sched.runFrame(reg, 1.0f / 60.0f);
    if (g_chan_completed != 0) { disconnectAll(); return fail(name, "channel completed early"); }
    if (!approx(reg.get<ChannelComponent>(e).progress, 2.0f / 60.0f, 0.002f)) { disconnectAll(); return fail(name, "progress wrong mid-channel"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    if (g_chan_completed != 1) { disconnectAll(); return fail(name, "channel did not complete on tick 3"); }
    if (reg.get<ChannelComponent>(e).active) { disconnectAll(); return fail(name, "channel still active after complete"); }
    // cancel resets
    if (!Gameplay::startChannel(reg, e, 7, 10.0f)) { disconnectAll(); return fail(name, "restart channel failed"); }
    sched.runFrame(reg, 1.0f / 60.0f);
    if (reg.get<ChannelComponent>(e).progress <= 0.0f) { disconnectAll(); return fail(name, "progress did not advance before cancel"); }
    if (!Gameplay::cancelChannel(reg, e, 5)) { disconnectAll(); return fail(name, "cancelChannel failed"); }
    if (g_chan_cancelled != 1) { disconnectAll(); return fail(name, "cancel event not emitted"); }
    const auto& after = reg.get<ChannelComponent>(e);
    if (after.active || !approx(after.progress, 0.0f)) { disconnectAll(); return fail(name, "cancel did not reset progress/active"); }
    if (Gameplay::cancelChannel(reg, e, 6)) { disconnectAll(); return fail(name, "cancel of inactive channel succeeded"); }
    (void)ch;
    disconnectAll();
    return pass(name);
}

bool testTeamRoster()
{
    const std::string name = "team roster query counts";
    entt::registry reg;
    auto mk = [&](uint8_t t) {
        auto e = reg.create();
        reg.emplace<TeamComponent>(e, TeamComponent{t});
        return e;
    };
    auto a1 = mk(2); auto a2 = mk(2); auto a3 = mk(2);
    auto b1 = mk(3); auto b2 = mk(3);
    mk(0);
    auto no_team = reg.create();
    if (Gameplay::countTeamMembers(reg, 2) != 3) return fail(name, "team 2 count != 3");
    if (Gameplay::countTeamMembers(reg, 3) != 2) return fail(name, "team 3 count != 2");
    if (Gameplay::countTeamMembers(reg, 0) != 1) return fail(name, "team 0 count != 1");
    if (Gameplay::countTeamMembers(reg, 99) != 0) return fail(name, "unknown team count != 0");
    if (Gameplay::getTeamMembers(reg, 2).size() != 3) return fail(name, "roster size != 3");
    if (countTeamMembers(reg, 3) != 2) return fail(name, "free countTeamMembers != 2");
    if (!Gameplay::areTeammates(reg, a1, a2)) return fail(name, "areTeammates false for same team");
    if (Gameplay::areTeammates(reg, a1, b1)) return fail(name, "areTeammates true for different teams");
    if (Gameplay::areTeammates(reg, a1, no_team)) return fail(name, "areTeammates true for no-team entity");
    if (!Gameplay::isOnTeam(reg, b2, 3) || Gameplay::isOnTeam(reg, b2, 2)) return fail(name, "isOnTeam wrong");
    (void)a3;
    return pass(name);
}

bool testReflection()
{
    const std::string name = "gameplay primitives reflected and registered";
    ReflectionRegistry reg;
    registerEngineReflection(reg);
    for (const char* n : {"TeamComponent", "CountdownTimerComponent", "ResourceWalletComponent",
                          "InventoryComponent", "HealthComponent", "ChannelComponent"})
    {
        if (!reg.findByName(n))
            return fail(name, std::string("missing reflection for ") + n);
    }
    const auto* team = reg.findByName("TeamComponent");
    bool has_team_id = false;
    for (auto& p : team->properties) if (p.name == "team_id") has_team_id = true;
    if (!has_team_id) return fail(name, "TeamComponent.team_id not reflected");
    const auto* health = reg.findByName("HealthComponent");
    bool has_inv = false;
    for (auto& p : health->properties) if (p.name == "invulnerable_until_tick") has_inv = true;
    if (!has_inv) return fail(name, "HealthComponent.invulnerable_until_tick not reflected");
    return pass(name);
}

bool testSchedulerRegistration()
{
    const std::string name = "gameplay systems registered in Gameplay phase";
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    registerGameplaySystems(sched);
    if (!sched.hasSystem("engine.gameplay.countdown_timers")) return fail(name, "countdown missing");
    if (!sched.hasSystem("engine.gameplay.channels")) return fail(name, "channels missing");
    bool cd_gameplay = false, ch_gameplay = false;
    for (auto& s : sched.executionOrder(Phase::Gameplay))
    {
        if (s == "engine.gameplay.countdown_timers") cd_gameplay = true;
        if (s == "engine.gameplay.channels") ch_gameplay = true;
    }
    if (!cd_gameplay || !ch_gameplay) return fail(name, "systems not in Gameplay order");
    return pass(name);
}
} // namespace

int main()
{
    int failures = 0;
    if (!testWallet()) ++failures;
    if (!testInventory()) ++failures;
    if (!testDamage()) ++failures;
    if (!testTimerExactTick()) ++failures;
    if (!testChannel()) ++failures;
    if (!testTeamRoster()) ++failures;
    if (!testReflection()) ++failures;
    if (!testSchedulerRegistration()) ++failures;
    if (failures == 0)
        std::cout << "All GameplayPrimitivesTests passed" << std::endl;
    else
        std::cerr << failures << " GameplayPrimitivesTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
