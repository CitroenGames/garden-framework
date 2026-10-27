#pragma once

#include "Components/Components.hpp"
#include "CompetitiveLogic.hpp"

#include <algorithm>
#include <cstdint>

// FPSShooter semantics layered onto the engine's generic trigger occupancy.
struct BuyZoneComponent
{
    uint8_t team_id = 0;
};

namespace Competitive {

// Buy permission is based on the server's post-physics occupant lists, scoped
// to the player's current team. TriggerOverlapState occupants are sorted by the
// engine, so lookup stays deterministic and avoids a per-query copy.
inline bool isInBuyZone(entt::registry& registry, entt::entity actor, uint8_t team_id)
{
    if (!registry.valid(actor) ||
        (team_id != teamId(FpsTeam::Attackers) && team_id != teamId(FpsTeam::Defenders)))
        return false;

    auto view = registry.view<BuyZoneComponent, TriggerComponent, TriggerOverlapState>();
    for (entt::entity zone_entity : view)
    {
        const auto& zone = view.get<BuyZoneComponent>(zone_entity);
        if (zone.team_id != team_id || !view.get<TriggerComponent>(zone_entity).enabled)
            continue;

        const auto& occupants = view.get<TriggerOverlapState>(zone_entity).occupants;
        if (std::binary_search(occupants.begin(), occupants.end(), actor))
            return true;
    }

    return false;
}

} // namespace Competitive
