#pragma once

#include <entt/entt.hpp>

namespace AnimationSystem
{
    // Update all entities with AnimationComponent
    // Advances blender, computes bone matrices from skeleton
    void update(entt::registry& registry, float dt);
}
