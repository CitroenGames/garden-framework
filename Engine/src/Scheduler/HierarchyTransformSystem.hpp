#pragma once

#include "ECS/Hierarchy.hpp"
#include "Scheduler/SystemScheduler.hpp"

#include <utility>

// Adds generic ECS hierarchy propagation to the fixed Transform phase.
// All hosts use the same registration function so their cached world poses
// follow the same scheduler boundary.
inline bool registerHierarchyTransformSystem(SystemScheduler& scheduler)
{
    SystemDescriptor descriptor;
    descriptor.name = "engine.hierarchy";
    descriptor.phase = Phase::Transform;
    descriptor.update = [](entt::registry& registry,
                           const Tick::SimulationTickContext&) {
        Hierarchy::updateTransformPropagation(registry);
    };
    return scheduler.addSystem(std::move(descriptor));
}
