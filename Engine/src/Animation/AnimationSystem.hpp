#pragma once

#include "EngineExport.h"

#include <entt/entt.hpp>

namespace AnimationSystem
{
// Evaluates runtime playback into an ECS pose. Set build_skinning_palette false
// for headless/server scheduling so pose state is available without palette work.
ENGINE_API void update(entt::registry& registry, float dt, bool build_skinning_palette = true);
}
