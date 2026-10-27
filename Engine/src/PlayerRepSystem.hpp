#pragma once

#include "Components/Components.hpp"
#include "ECS/Hierarchy.hpp"
#include <entt/entt.hpp>
#include <cmath>
#include <optional>

inline void apply_tracked_render_yaw(const glm::mat4& world_pose,
                                     TransformComponent& representation)
{
    const glm::mat3 basis(world_pose);
    const glm::vec3 forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    if (glm::dot(glm::vec2(forward.x, forward.z),
                 glm::vec2(forward.x, forward.z)) > 1e-8f)
    {
        representation.rotation.y = glm::degrees(std::atan2(-forward.x, -forward.z));
        return;
    }

    // A vertical forward vector has no horizontal heading. Recover yaw from
    // the right axis when available; otherwise retain the prior representation.
    const glm::vec3 right = basis * glm::vec3(1.0f, 0.0f, 0.0f);
    if (glm::dot(glm::vec2(right.x, right.z),
                 glm::vec2(right.x, right.z)) > 1e-8f)
        representation.rotation.y = glm::degrees(std::atan2(-right.z, right.x));
}

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

        const entt::entity tracked = resolveEntityRef(registry, pr.tracked_player);
        if (tracked != entt::null && registry.valid(tracked) &&
            registry.all_of<TransformComponent>(tracked))
        {
            const std::optional<glm::mat4> target_pose =
                Hierarchy::resolveRenderWorldMatrix(registry, tracked);
            if (target_pose)
            {
                // Keep the established world-space offset and yaw-only
                // representation behavior, sourced from the fully composed
                // tracked render pose.
                trans.position = glm::vec3((*target_pose)[3]) + pr.position_offset;
                apply_tracked_render_yaw(*target_pose, trans);
            }
        }

        // Visibility
        if (pr.visible_only_freecam && mesh_comp.m_mesh)
        {
            mesh_comp.m_mesh->visible = is_freecam;
        }
    }
}
