#pragma once

#include "Components/Components.hpp"
#include "SharedComponents.hpp"

#include <cstdint>
#include <limits>

// The runtime is registry context because it owns per-world allocation state,
// rather than per-item gameplay data. It intentionally has no transport or
// presentation bookkeeping; those are added only when the live publication
// and client presentation slices are connected.
struct WorldItemReplicationRuntime
{
    uint32_t next_id = 1;
    bool exhausted = false;

    uint32_t allocateId()
    {
        if (exhausted || next_id == 0)
            return 0;

        const uint32_t id = next_id;
        if (next_id == (std::numeric_limits<uint32_t>::max)()) {
            exhausted = true;
        } else {
            ++next_id;
        }
        return id;
    }
};

namespace FpsLogic {

inline WorldItemReplicationRuntime& worldItemReplicationRuntime(entt::registry& registry)
{
    if (auto* runtime = registry.ctx().find<WorldItemReplicationRuntime>())
        return *runtime;
    return registry.ctx().emplace<WorldItemReplicationRuntime>();
}

inline uint32_t allocateWorldItemReplicationId(entt::registry& registry)
{
    return worldItemReplicationRuntime(registry).allocateId();
}

inline void clearWorldItemReplicationRuntime(entt::registry& registry)
{
    registry.ctx().erase<WorldItemReplicationRuntime>();
}

} // namespace FpsLogic
