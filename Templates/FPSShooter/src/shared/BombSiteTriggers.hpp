#pragma once

#include "Components/Components.hpp"

#include <algorithm>
#include <cstdint>

// FPSShooter semantic data layered onto the engine's generic trigger
// occupancy. The engine knows only TriggerComponent and TriggerOverlapState.
struct BombSiteComponent
{
    uint8_t site_index = 0;
};

namespace Competitive {

// Resolve an actor's authored site from the generic post-physics occupant
// lists. Site ids are stable and the lowest id wins if malformed data causes
// overlapping volumes.
inline bool findBombSiteForOccupant(entt::registry& registry, entt::entity actor, uint8_t& out_site)
{
    if (!registry.valid(actor))
        return false;

    bool found = false;
    uint8_t selected = 0;
    auto view = registry.view<BombSiteComponent, TriggerComponent, TriggerOverlapState>();
    for (entt::entity site_entity : view)
    {
        if (!view.get<TriggerComponent>(site_entity).enabled)
            continue;

        const auto& occupants = view.get<TriggerOverlapState>(site_entity).occupants;
        if (std::find(occupants.begin(), occupants.end(), actor) == occupants.end())
            continue;

        const uint8_t site_index = view.get<BombSiteComponent>(site_entity).site_index;
        if (!found || site_index < selected)
        {
            found = true;
            selected = site_index;
        }
    }

    if (found)
        out_site = selected;
    return found;
}

} // namespace Competitive
