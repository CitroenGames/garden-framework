#pragma once

#include "EngineExport.h"
#include "Tick/TickSystem.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <entt/entt.hpp>

class EntityLifecycle;

// Phase model: plan/engine/core/system_scheduler.md section 2 (decided D5).
// Fixed phases run once per fixed step; variable phases once per frame.
// Hosts differ only in which systems are registered, never in phase order.
enum class Phase : uint8_t
{
    FrameBegin,
    PrePhysics,
    Physics,
    Gameplay,
    Transform,
    Presentation,
    FrameEnd,
};

constexpr size_t kPhaseCount = 7;
constexpr size_t phaseIndex(Phase p) { return static_cast<size_t>(p); }
inline bool isFixedPhase(Phase p)
{
    return p == Phase::PrePhysics || p == Phase::Physics ||
           p == Phase::Gameplay || p == Phase::Transform;
}

using SystemFn = std::function<void(entt::registry&, const Tick::SimulationTickContext&)>;

struct SystemDescriptor
{
    std::string name;
    std::string source_id = "engine";
    Phase phase = Phase::Gameplay;
    SystemFn update;
    std::vector<std::string> run_after;
    std::vector<std::string> run_before;
    bool enabled = true;
    bool run_on_server = true;
    bool run_on_client = true;
    bool run_when_paused = false;
};

// Generic per-host scheduler (EngineCore, no game/product types).
// Owns one FixedTickAccumulator, the two EventBus flush points
// (start of FrameBegin, end of FrameEnd; events.md 3.2) and the
// EntityLifecycle drain points (before PrePhysics per fixed step and
// at FrameEnd; system_scheduler.md section 2 frame algorithm).
// When register_builtin_timers is set, "engine.timers" is registered in
// Gameplay and ticked with fixed_delta (D5 rule 3).
class ENGINE_API SystemScheduler
{
public:
    explicit SystemScheduler(bool is_server = false,
                             float fixed_delta = 1.0f / 60.0f,
                             bool register_builtin_timers = true);
    ~SystemScheduler();
    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    bool addSystem(SystemDescriptor descriptor);
    bool removeSystem(std::string_view name);
    void removeBySource(std::string_view source_id);
    bool setEnabled(std::string_view name, bool on);
    bool isEnabled(std::string_view name) const;
    bool hasSystem(std::string_view name) const;
    size_t systemCount() const { return m_systems.size(); }

    const std::vector<std::string>& executionOrder(Phase phase) const
    {
        return m_order[phaseIndex(phase)];
    }

    void setLifecycle(EntityLifecycle* lifecycle) { m_lifecycle = lifecycle; }
    EntityLifecycle* lifecycle() const { return m_lifecycle; }
    void setManageEventBus(bool on) { m_manage_event_bus = on; }
    void setPaused(bool paused) { m_paused = paused; }
    bool isPaused() const { return m_paused; }
    bool isServer() const { return m_is_server; }
    void setFixedDelta(float dt);
    float getFixedDelta() const { return m_accumulator.getFixedDelta(); }
    float getInterpolationAlpha() const { return m_accumulator.getAlpha(); }
    uint32_t getTick() const { return m_tick; }
    double getWorldTime() const { return m_world_time; }
    Tick::FixedTickAccumulator& accumulator() { return m_accumulator; }

    // Runs one frame; returns the number of fixed steps consumed.
    uint32_t runFrame(entt::registry& registry, float frame_dt);

private:
    struct Entry
    {
        SystemDescriptor desc;
        uint64_t registration_index = 0;
    };
    bool resolveOrder();
    bool wouldKeepOrderValid(const Entry& candidate) const;
    void runPhase(entt::registry& r, Phase phase, const Tick::SimulationTickContext& ctx, bool paused_only);
    Tick::SimulationTickContext frameContext() const;

    std::vector<Entry> m_systems;
    std::vector<std::string> m_order[kPhaseCount];
    std::vector<const Entry*> m_sorted[kPhaseCount];
    Tick::FixedTickAccumulator m_accumulator;
    EntityLifecycle* m_lifecycle = nullptr;
    uint64_t m_next_index = 0;
    uint32_t m_tick = 0;
    double m_world_time = 0.0;
    bool m_is_server = false;
    bool m_paused = false;
    bool m_manage_event_bus = true;
};
