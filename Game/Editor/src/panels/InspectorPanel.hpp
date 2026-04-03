#pragma once

#include <entt/entt.hpp>
#include <unordered_map>
#include <string>
#include "imgui.h"

class InspectorPanel
{
public:
    // entity -> original mesh file path (ECS holds GPU ptr, not a string)
    std::unordered_map<entt::entity, std::string> mesh_path_cache;

    // Draw the inspector for the selected entity.
    // Returns true if any transform was modified (so caller can mark BVH dirty).
    bool draw(entt::registry& registry, entt::entity selected);

private:
    // Helper to draw a removable component header with an accent color bar.
    // Returns true if the section is open.
    // Sets *removed = true if the user clicks the remove button.
    bool drawComponentHeader(const char* label, bool can_remove, bool* removed,
                             ImVec4 accent_color = ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
};
