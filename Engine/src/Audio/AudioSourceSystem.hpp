#pragma once
#include "EngineExport.h"
#include <entt/entt.hpp>

// Client-only presentation system consuming AudioSourceComponent.
// Call from the Presentation phase on clients; never register on a
// dedicated server (run_on_server=false descriptors enforce this).
namespace AudioSourceSystem
{
    ENGINE_API void update(entt::registry& registry, float dt);
    ENGINE_API void shutdown(entt::registry& registry);
}
