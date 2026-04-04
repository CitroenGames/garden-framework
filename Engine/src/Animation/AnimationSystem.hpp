#pragma once

#include "EngineExport.h"
#include <entt/entt.hpp>

namespace AnimationSystem
{
    // Update all entities with AnimationComponent
    // Advances blender, computes bone matrices from skeleton
    ENGINE_API void update(entt::registry& registry, float dt);
}
