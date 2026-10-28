#pragma once

#include "EngineExport.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string_view>

namespace AnimGraphSystem
{
// Fixed-tick mutation path. It owns graph instance initialization and clocks;
// render/presentation code must never call it with a variable frame delta.
ENGINE_API void advance(entt::registry& registry, float fixed_dt);

// Presentation output only. This writes transient AnimPoseComponent data but
// does not change clocks, parameters, or active machine states.
ENGINE_API void evaluate(entt::registry& registry, bool build_skinning_palette = true);

ENGINE_API bool setBool(entt::registry& registry, entt::entity entity,
                        std::string_view parameter, bool value);
ENGINE_API bool setInt(entt::registry& registry, entt::entity entity,
                       std::string_view parameter, int32_t value);
ENGINE_API bool setFloat(entt::registry& registry, entt::entity entity,
                         std::string_view parameter, float value);
ENGINE_API bool setVec3(entt::registry& registry, entt::entity entity,
                        std::string_view parameter, const glm::vec3& value);
ENGINE_API int activeState(const entt::registry& registry, entt::entity entity,
                           std::string_view machine);
}
