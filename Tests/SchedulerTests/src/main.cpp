#include "Scheduler/SystemScheduler.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Network/ServerNetworkManager.hpp"
#include "Timer/TimerSystem.hpp"
#include "world.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

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

SystemDescriptor makeSys(const std::string& name, Phase phase, SystemFn fn)
{
    SystemDescriptor d;
    d.name = name;
    d.phase = phase;
    d.update = std::move(fn);
    return d;
}

// S1: topo order C(before A), A, B(after A) in Gameplay
bool testS1()
{
    const std::string name = "S1 topological order within phase";
    entt::registry reg;
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    auto noop = [](entt::registry&, const Tick::SimulationTickContext&) {};
    if (!sched.addSystem(makeSys("A", Phase::Gameplay, noop))) return fail(name, "add A failed");
    SystemDescriptor b = makeSys("B", Phase::Gameplay, noop);
    b.run_after = {"A"};
    if (!sched.addSystem(std::move(b))) return fail(name, "add B failed");
    SystemDescriptor c = makeSys("C", Phase::Gameplay, noop);
    c.run_before = {"A"};
    if (!sched.addSystem(std::move(c))) return fail(name, "add C failed");
    const auto& order = sched.executionOrder(Phase::Gameplay);
    if (order != std::vector<std::string>({"C", "A", "B"}))
        return fail(name, "executionOrder != [C,A,B]");
    if (sched.addSystem(makeSys("A", Phase::Gameplay, noop)))
        return fail(name, "duplicate name accepted");
    SystemDescriptor bad = makeSys("D", Phase::Gameplay, noop);
    bad.run_after = {"missing.system"};
    if (sched.addSystem(std::move(bad)))
        return fail(name, "unknown dependency accepted");
    return pass(name);
}

// S2: cycle rejected
bool testS2()
{
    const std::string name = "S2 dependency cycle rejected";
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    auto noop = [](entt::registry&, const Tick::SimulationTickContext&) {};
    SystemDescriptor a = makeSys("A", Phase::Gameplay, noop);
    a.run_after = {"B"};
    // B not yet present -> unknown dep must fail
    if (sched.addSystem(a)) return fail(name, "A(after missing B) accepted");
    if (!sched.addSystem(makeSys("B", Phase::Gameplay, noop))) return fail(name, "add B failed");
    SystemDescriptor a2 = makeSys("A", Phase::Gameplay, noop);
    a2.run_after = {"B"};
    if (!sched.addSystem(std::move(a2))) return fail(name, "add A after B failed");
    SystemDescriptor cyc = makeSys("C", Phase::Gameplay, noop);
    // Force a cycle: C after A, then try to make B after C would cycle,
    // simulated by adding a system that closes loop via run_before.
    cyc.run_after = {"A"};
    if (!sched.addSystem(std::move(cyc))) return fail(name, "add C failed");
    // Now attempt a system that would create cycle A->...->it->A
    SystemDescriptor loop = makeSys("D", Phase::Gameplay, noop);
    loop.run_after = {"C"};
    loop.run_before = {"B"};
    if (sched.addSystem(std::move(loop)))
        return fail(name, "cycle-creating system accepted");
    return pass(name);
}

// Phase order recording across one frame with 1 fixed step
bool testPhaseOrder()
{
    const std::string name = "Phase order execution recording";
    entt::registry reg;
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    std::vector<std::string> seq;
    auto rec = [&seq](const char* label) {
        return [&seq, label](entt::registry&, const Tick::SimulationTickContext&) {
            seq.push_back(label);
        };
    };
    sched.addSystem(makeSys("s.framebegin", Phase::FrameBegin, rec("FrameBegin")));
    sched.addSystem(makeSys("s.prephysics", Phase::PrePhysics, rec("PrePhysics")));
    sched.addSystem(makeSys("s.physics", Phase::Physics, rec("Physics")));
    sched.addSystem(makeSys("s.gameplay", Phase::Gameplay, rec("Gameplay")));
    sched.addSystem(makeSys("s.transform", Phase::Transform, rec("Transform")));
    sched.addSystem(makeSys("s.presentation", Phase::Presentation, rec("Presentation")));
    sched.addSystem(makeSys("s.frameend", Phase::FrameEnd, rec("FrameEnd")));
    uint32_t steps = sched.runFrame(reg, 1.0f / 60.0f);
    if (steps != 1) return fail(name, "expected 1 step");
    std::vector<std::string> expected = {"FrameBegin", "PrePhysics", "Physics",
                                         "Gameplay", "Transform", "Presentation", "FrameEnd"};
    if (seq != expected) return fail(name, "phase sequence mismatch");
    return pass(name);
}

// S3 + task fixed-dt step count
bool testFixedSteps()
{
    const std::string name = "S3 fixed-step count over 1s at 60Hz";
    entt::registry reg;
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    int calls = 0;
    uint32_t last_tick = 0;
    bool ordered = true;
    sched.addSystem(makeSys("counter", Phase::Gameplay,
        [&](entt::registry&, const Tick::SimulationTickContext& ctx) {
            if (calls > 0 && ctx.tick_index != last_tick + 1) ordered = false;
            last_tick = ctx.tick_index;
            ++calls;
        }));
    for (int i = 0; i < 60; ++i) sched.runFrame(reg, 1.0f / 60.0f);
    if (calls != 60) return fail(name, "expected 60 calls, got " + std::to_string(calls));
    if (!ordered) return fail(name, "tick_index not sequential");
    // Zero-step frame: no fixed calls
    sched.runFrame(reg, 0.001f);
    if (calls != 60) return fail(name, "zero-step frame invoked fixed system");
    // Multi-step frame
    sched.runFrame(reg, 3.0f / 60.0f);
    if (calls != 63) return fail(name, "expected 63 after 3-step frame");
    return pass(name);
}

// S4 disabled skipped, resumes without burst
bool testDisabled()
{
    const std::string name = "S4 disabled system skipped";
    entt::registry reg;
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    int calls = 0;
    sched.addSystem(makeSys("sys", Phase::Gameplay,
        [&](entt::registry&, const Tick::SimulationTickContext&) { ++calls; }));
    sched.runFrame(reg, 1.0f / 60.0f);
    if (calls != 1) return fail(name, "expected 1 call");
    if (!sched.setEnabled("sys", false)) return fail(name, "setEnabled failed");
    sched.runFrame(reg, 1.0f / 60.0f);
    sched.runFrame(reg, 1.0f / 60.0f);
    if (calls != 1) return fail(name, "disabled system invoked");
    if (sched.setEnabled("unknown", true)) return fail(name, "setEnabled unknown succeeded");
    sched.setEnabled("sys", true);
    sched.runFrame(reg, 1.0f / 60.0f);
    if (calls != 2) return fail(name, "re-enabled burst or no resume, calls=" + std::to_string(calls));
    return pass(name);
}

// S6 host filter
bool testHostFilter()
{
    const std::string name = "S6 server host filter skips Presentation-only system";
    entt::registry reg;
    SystemScheduler server(true, 1.0f / 60.0f, false);
    int pres = 0, game = 0;
    SystemDescriptor p = makeSys("pres", Phase::Presentation,
        [&](entt::registry&, const Tick::SimulationTickContext&) { ++pres; });
    p.run_on_server = false;
    server.addSystem(std::move(p));
    server.addSystem(makeSys("game", Phase::Gameplay,
        [&](entt::registry&, const Tick::SimulationTickContext&) { ++game; }));
    server.runFrame(reg, 1.0f / 60.0f);
    if (pres != 0) return fail(name, "presentation ran on server");
    if (game != 1) return fail(name, "gameplay did not run on server");
    return pass(name);
}

// Server physics is one real world/Jolt step in Physics before gameplay for
// every fixed scheduler tick, including all steps consumed by catch-up frames.
bool testServerPhysicsBeforeGameplayCatchUp()
{
    const std::string name = "server physics precedes gameplay once per catch-up tick";
    world server_world;
    server_world.initializePhysics();

    const entt::entity falling_body = server_world.registry.create();
    server_world.registry.emplace<TransformComponent>(falling_body, 0.0f, 10.0f, 0.0f);
    auto& rigid_body = server_world.registry.emplace<RigidBodyComponent>(falling_body);
    rigid_body.mass = 1.0f;
    rigid_body.apply_gravity = true;
    auto& collider = server_world.registry.emplace<ColliderComponent>(falling_body);
    collider.shape_type = ColliderShapeType::Box;
    collider.box_half_extents = glm::vec3(0.5f);
    const auto shape = PhysicsSystem::createShapeFromCollider(collider, glm::vec3(1.0f));
    if (!shape || server_world.getPhysicsSystem().createDynamicBody(
            glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), shape, falling_body).IsInvalid())
        return fail(name, "failed to create the Jolt-backed fixture body");

    SystemScheduler server(true, server_world.getFixedDelta(), false);
    Net::ServerNetworkManager server_network;
    std::vector<std::string> trace;
    uint32_t ingress_calls = 0;
    uint32_t gameplay_calls = 0;
    uint32_t applied_input_tick = (std::numeric_limits<uint32_t>::max)();
    bool phase_order_valid = true;
    SystemDescriptor ingress = makeSys("game.server_pre_physics", Phase::PrePhysics,
        [&](entt::registry&, const Tick::SimulationTickContext& ctx) {
            if (ctx.tick_index != server_world.getSimulationTick())
                phase_order_valid = false;
            applied_input_tick = ctx.tick_index;
            trace.push_back("ingress:" + std::to_string(ctx.tick_index));
            ++ingress_calls;
        });
    if (!server.addSystem(std::move(ingress)))
        return fail(name, "failed to register server ingress system");

    SystemDescriptor physics = makeSys("engine.physics", Phase::Physics,
        [&](entt::registry&, const Tick::SimulationTickContext& ctx) {
            if (applied_input_tick != ctx.tick_index || trace.empty() ||
                trace.back() != "ingress:" + std::to_string(ctx.tick_index))
                phase_order_valid = false;
            server_world.stepPhysicsSingleStep();
            trace.push_back("physics:" + std::to_string(ctx.tick_index));
        });
    if (!server.addSystem(std::move(physics)))
        return fail(name, "failed to register server Physics system");

    SystemDescriptor gameplay = makeSys("game.server_update", Phase::Gameplay,
        [&](entt::registry&, const Tick::SimulationTickContext& ctx) {
            if (server_world.getSimulationTick() != ctx.tick_index + 1 ||
                trace.empty() || trace.back() != "physics:" + std::to_string(ctx.tick_index) ||
                applied_input_tick != ctx.tick_index)
                phase_order_valid = false;
            server_network.advanceSimulationTicks(1);
            if (server_network.getCurrentTick() != server_world.getSimulationTick())
                phase_order_valid = false;
            trace.push_back("gameplay:" + std::to_string(ctx.tick_index));
            ++gameplay_calls;
        });
    if (!server.addSystem(std::move(gameplay)))
        return fail(name, "failed to register server gameplay system");

    const float fixed_delta = server_world.getFixedDelta();
    if (server.runFrame(server_world.registry, fixed_delta * 0.5f) != 0)
        return fail(name, "partial frame unexpectedly consumed a fixed tick");
    if (server.runFrame(server_world.registry, fixed_delta * 2.5f) != 3)
        return fail(name, "catch-up frame did not consume three fixed ticks");
    if (server.runFrame(server_world.registry, fixed_delta * 2.0f) != 2)
        return fail(name, "second catch-up frame did not consume two fixed ticks");

    if (!phase_order_valid)
        return fail(name, "ingress, Physics, and Gameplay were not aligned once per tick");
    if (server_world.getSimulationTick() != 5 || server_network.getCurrentTick() != 5 ||
        server.getTick() != 5 || ingress_calls != 5 || gameplay_calls != 5)
        return fail(name, "scheduler, ingress, world, network, and gameplay ticks did not match five consumed ticks");
    if (std::abs(server_world.getWorldTime() - static_cast<double>(fixed_delta) * 5.0) > 1.0e-6)
        return fail(name, "world time did not advance by exactly one fixed delta per consumed tick");
    if (trace.size() != 15)
        return fail(name, "expected one ingress, Physics, and Gameplay event per consumed tick");
    for (uint32_t tick = 0; tick < 5; ++tick)
    {
        if (trace[tick * 3] != "ingress:" + std::to_string(tick) ||
            trace[tick * 3 + 1] != "physics:" + std::to_string(tick) ||
            trace[tick * 3 + 2] != "gameplay:" + std::to_string(tick))
            return fail(name, "ingress, Physics, and Gameplay order or tick identity was incorrect");
    }
    if (server_world.registry.get<TransformComponent>(falling_body).position.y >= 10.0f)
        return fail(name, "catch-up scheduler frames did not advance the Jolt body");

    return pass(name);
}

int g_destroyed = 0;
int g_created = 0;
void onDestroyedEvt(const EntityDestroyedEvent&) { ++g_destroyed; }
void onCreatedEvt(const EntityCreatedEvent&) { ++g_created; }

// S5 destroy during Gameplay drained at boundary + event delivered
bool testDestroyDrain()
{
    const std::string name = "S5 destroy in Gameplay drained at FrameEnd, event delivered";
    EventBus::get().clear();
    g_destroyed = 0; g_created = 0;
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedEvt>();
    EventBus::get().sink<EntityCreatedEvent>().connect<&onCreatedEvt>();
    entt::registry reg;
    EntityLifecycle life(reg, 1234);
    SystemScheduler sched(false, 1.0f / 60.0f, false);
    sched.setLifecycle(&life);
    entt::entity victim = life.create("victim");
    bool valid_in_transform = false;
    bool valid_in_gameplay_after_destroy = false;
    sched.addSystem(makeSys("killer", Phase::Gameplay,
        [&](entt::registry& r, const Tick::SimulationTickContext&) {
            life.destroy(victim);
            valid_in_gameplay_after_destroy = r.valid(victim);
        }));
    sched.addSystem(makeSys("checker", Phase::Transform,
        [&](entt::registry& r, const Tick::SimulationTickContext&) {
            valid_in_transform = r.valid(victim);
        }));
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedEvt>();
    EventBus::get().sink<EntityCreatedEvent>().disconnect<&onCreatedEvt>();
    EventBus::get().clear();
    if (!valid_in_gameplay_after_destroy) return fail(name, "entity invalid immediately after destroy()");
    if (!valid_in_transform) return fail(name, "entity invalid in Transform (drained too early)");
    if (reg.valid(victim)) return fail(name, "entity still valid after FrameEnd drain");
    if (g_destroyed != 1) return fail(name, "destroyed event not delivered, count=" + std::to_string(g_destroyed));
    if (g_created != 1) return fail(name, "created event not flushed, count=" + std::to_string(g_created));
    return pass(name);
}

int g_timer_fired = 0;
int g_timer_expired_events = 0;
void onTimerExpired(const TimerExpiredEvent&) { ++g_timer_expired_events; }

// Timer expiry on fixed tick (D5-T6 style: same tick at different frame shapes)
bool testTimerFixed()
{
    const std::string name = "Timer expiry on fixed tick";
    TimerSystem::get().clear();
    EventBus::get().clear();
    g_timer_fired = 0; g_timer_expired_events = 0;
    EventBus::get().sink<TimerExpiredEvent>().connect<&onTimerExpired>();
    // 0.05s = 3 fixed ticks at 60Hz
    TimerSystem::get().createTimer(0.05f, [](TimerId) { ++g_timer_fired; }, false);
    entt::registry reg;
    SystemScheduler sched(false, 1.0f / 60.0f, true); // auto engine.timers
    sched.runFrame(reg, 1.0f / 60.0f);
    sched.runFrame(reg, 1.0f / 60.0f);
    if (g_timer_fired != 0) return fail(name, "timer fired early");
    sched.runFrame(reg, 1.0f / 60.0f);
    EventBus::get().sink<TimerExpiredEvent>().disconnect<&onTimerExpired>();
    if (g_timer_fired != 1) return fail(name, "timer did not fire on tick 3");
    if (g_timer_expired_events < 1) return fail(name, "TimerExpiredEvent not delivered");

    // Different frame shape: one 0.0333 frame (2 steps) + one 0.0167 (1 step)
    TimerSystem::get().clear();
    EventBus::get().clear();
    g_timer_fired = 0;
    TimerSystem::get().createTimer(0.05f, [](TimerId) { ++g_timer_fired; }, false);
    entt::registry reg2;
    SystemScheduler sched2(false, 1.0f / 60.0f, true);
    sched2.runFrame(reg2, 2.0f / 60.0f);
    if (g_timer_fired != 0) return fail(name, "shape2 timer fired early");
    sched2.runFrame(reg2, 1.0f / 60.0f);
    if (g_timer_fired != 1) return fail(name, "shape2 timer did not fire after 3 ticks");
    TimerSystem::get().clear();
    EventBus::get().clear();
    return pass(name);
}

} // namespace

int main()
{
    int failures = 0;
    if (!testS1()) ++failures;
    if (!testS2()) ++failures;
    if (!testPhaseOrder()) ++failures;
    if (!testFixedSteps()) ++failures;
    if (!testDisabled()) ++failures;
    if (!testHostFilter()) ++failures;
    if (!testServerPhysicsBeforeGameplayCatchUp()) ++failures;
    if (!testDestroyDrain()) ++failures;
    if (!testTimerFixed()) ++failures;
    if (failures == 0)
        std::cout << "All SchedulerTests passed" << std::endl;
    else
        std::cerr << failures << " SchedulerTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
