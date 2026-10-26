#include "Scheduler/SystemScheduler.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "Events/EventBus.hpp"
#include "Timer/TimerSystem.hpp"
#include "Utils/Log.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

SystemScheduler::SystemScheduler(bool is_server, float fixed_delta, bool register_builtin_timers)
    : m_accumulator(fixed_delta)
    , m_is_server(is_server)
{
    for (auto& v : m_order) v.clear();
    if (register_builtin_timers)
    {
        SystemDescriptor timers;
        timers.name = "engine.timers";
        timers.phase = Phase::Gameplay;
        timers.update = [](entt::registry&, const Tick::SimulationTickContext& ctx) {
            TimerSystem::get().update(ctx.fixed_delta);
        };
        addSystem(std::move(timers));
    }
}

SystemScheduler::~SystemScheduler() = default;

void SystemScheduler::setFixedDelta(float dt) { m_accumulator.setFixedDelta(dt); }

bool SystemScheduler::hasSystem(std::string_view name) const
{
    for (const auto& e : m_systems)
        if (e.desc.name == name) return true;
    return false;
}

bool SystemScheduler::isEnabled(std::string_view name) const
{
    for (const auto& e : m_systems)
        if (e.desc.name == name) return e.desc.enabled;
    return false;
}

bool SystemScheduler::setEnabled(std::string_view name, bool on)
{
    for (auto& e : m_systems)
        if (e.desc.name == name) { e.desc.enabled = on; return true; }
    return false;
}

bool SystemScheduler::removeSystem(std::string_view name)
{
    for (size_t i = 0; i < m_systems.size(); ++i)
        if (m_systems[i].desc.name == name)
        {
            m_systems.erase(m_systems.begin() + static_cast<long>(i));
            resolveOrder();
            return true;
        }
    return false;
}

void SystemScheduler::removeBySource(std::string_view source_id)
{
    m_systems.erase(std::remove_if(m_systems.begin(), m_systems.end(),
        [&](const Entry& e) { return e.desc.source_id == source_id; }), m_systems.end());
    resolveOrder();
}

// Validate candidate against existing systems of the same phase plus itself.
bool SystemScheduler::wouldKeepOrderValid(const Entry& candidate) const
{
    // collect same-phase nodes including candidate
    std::vector<const Entry*> nodes;
    for (const auto& e : m_systems)
        if (e.desc.phase == candidate.desc.phase) nodes.push_back(&e);
    nodes.push_back(&candidate);
    auto findNode = [&](std::string_view n) -> const Entry* {
        for (auto* e : nodes) if (e->desc.name == n) return e;
        return nullptr;
    };
    // unknown dependency check (all nodes' deps must exist in same phase)
    for (auto* e : nodes)
    {
        for (const auto& dep : e->desc.run_after)
            if (!findNode(dep)) return false;
        for (const auto& dep : e->desc.run_before)
            if (!findNode(dep)) return false;
    }
    // cycle check via Kahn
    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (auto* e : nodes) indeg[e->desc.name] = 0;
    auto addEdge = [&](const std::string& from, const std::string& to) {
        edges[from].push_back(to); indeg[to]++;
    };
    for (auto* e : nodes)
    {
        for (const auto& dep : e->desc.run_after) addEdge(dep, e->desc.name);
        for (const auto& dep : e->desc.run_before) addEdge(e->desc.name, dep);
    }
    std::vector<std::string> queue;
    for (auto* e : nodes) if (indeg[e->desc.name] == 0) queue.push_back(e->desc.name);
    size_t seen = 0;
    while (!queue.empty())
    {
        std::string cur = queue.back(); queue.pop_back(); ++seen;
        for (auto& nxt : edges[cur]) if (--indeg[nxt] == 0) queue.push_back(nxt);
    }
    return seen == nodes.size();
}

bool SystemScheduler::resolveOrder()
{
    for (size_t p = 0; p < kPhaseCount; ++p)
    {
        m_order[p].clear(); m_sorted[p].clear();
        std::vector<const Entry*> nodes;
        for (const auto& e : m_systems)
            if (phaseIndex(e.desc.phase) == p) nodes.push_back(&e);
        std::sort(nodes.begin(), nodes.end(),
            [](const Entry* a, const Entry* b) { return a->registration_index < b->registration_index; });
        std::unordered_map<std::string, int> indeg;
        std::unordered_map<std::string, std::vector<std::string>> edges;
        std::unordered_map<std::string, const Entry*> byName;
        for (auto* e : nodes) { indeg[e->desc.name] = 0; byName[e->desc.name] = e; }
        auto addEdge = [&](const std::string& from, const std::string& to) {
            edges[from].push_back(to); indeg[to]++;
        };
        for (auto* e : nodes)
        {
            for (const auto& dep : e->desc.run_after) if (byName.count(dep)) addEdge(dep, e->desc.name);
            for (const auto& dep : e->desc.run_before) if (byName.count(dep)) addEdge(e->desc.name, dep);
        }
        // Kahn, always picking lowest registration index among ready (stable/deterministic)
        std::unordered_set<std::string> done;
        while (m_sorted[p].size() < nodes.size())
        {
            const Entry* best = nullptr;
            for (auto* e : nodes)
            {
                if (done.count(e->desc.name) || indeg[e->desc.name] != 0) continue;
                if (!best || e->registration_index < best->registration_index) best = e;
            }
            if (!best) break; // cycle; validated at add time, should not happen
            done.insert(best->desc.name);
            m_sorted[p].push_back(best);
            m_order[p].push_back(best->desc.name);
            for (auto& nxt : edges[best->desc.name]) indeg[nxt]--;
        }
    }
    return true;
}

bool SystemScheduler::addSystem(SystemDescriptor descriptor)
{
    if (descriptor.name.empty() || !descriptor.update)
    {
        LOG_ENGINE_ERROR("SystemScheduler: system with empty name or no update rejected");
        return false;
    }
    if (hasSystem(descriptor.name))
    {
        LOG_ENGINE_ERROR("SystemScheduler: duplicate system '{}' rejected", descriptor.name);
        return false;
    }
    Entry candidate;
    candidate.desc = std::move(descriptor);
    candidate.registration_index = m_next_index;
    if (!wouldKeepOrderValid(candidate))
    {
        LOG_ENGINE_ERROR("SystemScheduler: system '{}' rejected (unknown dependency or cycle)", candidate.desc.name);
        return false;
    }
    m_systems.push_back(std::move(candidate));
    ++m_next_index;
    resolveOrder();
    return true;
}

Tick::SimulationTickContext SystemScheduler::frameContext() const
{
    Tick::SimulationTickContext ctx;
    ctx.tick_index = m_tick;
    ctx.substep_index = 0;
    ctx.substep_count = 0;
    ctx.fixed_delta = m_accumulator.getFixedDelta();
    ctx.world_time = m_world_time;
    return ctx;
}

void SystemScheduler::runPhase(entt::registry& r, Phase phase, const Tick::SimulationTickContext& ctx, bool paused_only)
{
    // Copy pointers: a system may add/remove systems; pointers into m_systems
    // could invalidate. Snapshot descriptors' callables by name lookup per call.
    std::vector<std::string> names = m_order[phaseIndex(phase)];
    for (const auto& name : names)
    {
        const Entry* entry = nullptr;
        for (const auto& e : m_systems) if (e.desc.name == name) { entry = &e; break; }
        if (!entry || !entry->desc.enabled || !entry->desc.update) continue;
        if (m_is_server && !entry->desc.run_on_server) continue;
        if (!m_is_server && !entry->desc.run_on_client) continue;
        if (paused_only && !entry->desc.run_when_paused) continue;
        SystemFn fn = entry->desc.update;
        fn(r, ctx);
    }
}

uint32_t SystemScheduler::runFrame(entt::registry& registry, float frame_dt)
{
    if (m_paused)
    {
        if (m_manage_event_bus) EventBus::get().flush();
        runPhase(registry, Phase::Presentation, frameContext(), true);
        if (m_manage_event_bus) EventBus::get().flush();
        return 0;
    }

    // FrameBegin: flush point 1 (events.md 3.2)
    if (m_manage_event_bus) EventBus::get().flush();
    runPhase(registry, Phase::FrameBegin, frameContext(), false);

    const uint32_t steps = m_accumulator.consume(frame_dt);
    for (uint32_t i = 0; i < steps; ++i)
    {
        Tick::SimulationTickContext ctx;
        ctx.tick_index = m_tick;
        ctx.substep_index = i;
        ctx.substep_count = steps;
        ctx.fixed_delta = m_accumulator.getFixedDelta();
        ctx.world_time = m_world_time;

        if (m_lifecycle) m_lifecycle->drainDestroyQueue();
        runPhase(registry, Phase::PrePhysics, ctx, false);
        runPhase(registry, Phase::Physics, ctx, false);
        runPhase(registry, Phase::Gameplay, ctx, false);
        runPhase(registry, Phase::Transform, ctx, false);
        ++m_tick;
        m_world_time += ctx.fixed_delta;
    }

    runPhase(registry, Phase::Presentation, frameContext(), false);
    runPhase(registry, Phase::FrameEnd, frameContext(), false);
    if (m_lifecycle) m_lifecycle->drainDestroyQueue();
    // FrameEnd: flush point 2
    if (m_manage_event_bus) EventBus::get().flush();
    return steps;
}
