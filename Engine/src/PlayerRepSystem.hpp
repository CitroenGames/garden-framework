#pragma once

#include "Components/Components.hpp"
#include <entt/entt.hpp>

// Updates player representation entities to follow their tracked player.
// Called each frame during gameplay simulation.
inline void update_player_representations(entt::registry& registry, bool is_freecam)
{
    auto view = registry.view<PlayerRepresentationComponent, TransformComponent, MeshComponent>();

    for (entt::entity entity : view)
    {
        auto& pr    = view.get<PlayerRepresentationComponent>(entity);
        auto& trans  = view.get<TransformComponent>(entity);
        auto& mesh_comp = view.get<MeshComponent>(entity);

        if (registry.valid(pr.tracked_player) &&
            registry.all_of<TransformComponent>(pr.tracked_player))
        {
            const auto& target_trans = registry.get<TransformComponent>(pr.tracked_player);

            // Sync position with offset
            trans.position = target_trans.position + pr.position_offset;

            // Sync Y rotation for character representation
            trans.rotation.y = target_trans.rotation.y;
        }

        // Visibility
        if (pr.visible_only_freecam && mesh_comp.m_mesh)
        {
            mesh_comp.m_mesh->visible = is_freecam;
        }
    }
}
