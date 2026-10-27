#pragma once

#include "Components/HierarchyComponents.hpp"

#include <entt/entt.hpp>

namespace Net {

// Network snapshots carry TransformComponent values without hierarchy links.
// A non-null ParentComponent would therefore reinterpret a local pose as a
// world pose on another machine. Keep this rule at networking boundaries until
// a versioned parent-link reconstruction contract exists.
inline bool isNetworkHierarchyRoot(const entt::registry& registry, entt::entity entity)
{
    if (entity == entt::null || !registry.valid(entity)) {
        return false;
    }

    const auto* parent = registry.try_get<ParentComponent>(entity);
    return parent == nullptr || parent->parent.isNull();
}

} // namespace Net
