#pragma once

#include "EngineExport.h"

#include <entt/entt.hpp>

namespace AnimationResourceBindingSystem
{
// Runs before AnimationSystem. A failed replacement intentionally retains an
// earlier active immutable set and its rest-pose fallback.
ENGINE_API void update(entt::registry& registry);
}
