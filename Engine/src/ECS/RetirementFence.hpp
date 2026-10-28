#pragma once

#include <algorithm>
#include <vector>
#include <unordered_set>
#include <entt/entt.hpp>

// Internal world-owned fence. The active cohort is frozen before callbacks;
// pending requests may be added during callbacks and queries.
struct RetirementFence {
    std::vector<entt::entity> entities;
    std::unordered_set<entt::entity> pending;
    const void* rollback_context = nullptr;
    bool (*rollback_contains)(const void*, entt::entity) noexcept = nullptr;
    bool contains(entt::entity entity) const noexcept {
        return std::find(entities.begin(), entities.end(), entity) != entities.end();
    }
    bool isPending(entt::entity entity) const noexcept { return pending.find(entity) != pending.end(); }
    bool blocks(entt::entity entity) const noexcept { return contains(entity) || isPending(entity) ||
            (rollback_contains && rollback_contains(rollback_context, entity)); }
};

// Registry context borrows this pointer; world outlives the binding.
struct RetirementFenceBinding {
    const RetirementFence* fence = nullptr;
};
