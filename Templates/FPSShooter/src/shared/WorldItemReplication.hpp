#pragma once

#include "Components/Components.hpp"
#include "SharedComponents.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

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

// World-item messages are delivered only to actual transport peers. Synthetic
// server bot ownership IDs deliberately have no peer and must never produce a
// replay or publication send.
inline bool isWorldItemReplayEligible(bool has_real_peer)
{
    return has_real_peer;
}

// The server owns replay timing, but this collection is shared so its stable
// ID ordering is explicit and can be covered without a transport fixture.
inline std::vector<entt::entity> collectWorldItemsForReplay(entt::registry& registry)
{
    std::vector<entt::entity> dropped_items;
    auto item_view = registry.view<TransformComponent, DroppedWeaponComponent,
                                   WorldItemReplicationIdComponent>();
    for (const entt::entity dropped_item : item_view)
        dropped_items.push_back(dropped_item);

    std::sort(dropped_items.begin(), dropped_items.end(), [&registry](entt::entity lhs, entt::entity rhs) {
        return registry.get<WorldItemReplicationIdComponent>(lhs).value <
            registry.get<WorldItemReplicationIdComponent>(rhs).value;
    });
    return dropped_items;
}

inline void clearWorldItemReplicationRuntime(entt::registry& registry)
{
    registry.ctx().erase<WorldItemReplicationRuntime>();
}

} // namespace FpsLogic
