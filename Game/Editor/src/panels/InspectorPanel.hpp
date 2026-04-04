#pragma once

#include <entt/entt.hpp>
#include <unordered_map>
#include <string>
#include <glm/glm.hpp>
#include "imgui.h"

class InspectorPanel
{
public:
    // entity -> original mesh file path (ECS holds GPU ptr, not a string)
    std::unordered_map<entt::entity, std::string> mesh_path_cache;

    // Set by EditorApp each frame for LOD debug display
    glm::vec3 debug_cam_pos{0.0f};
    glm::mat4 debug_projection{1.0f};

    // Draw the inspector for the selected entity.
    // Returns true if any transform was modified (so caller can mark BVH dirty).
    // out_unsaved: set to true when any property is modified.
    // out_edit_started: set to true when a drag/edit operation begins (for undo snapshots).
    bool draw(entt::registry& registry, entt::entity selected,
              bool* out_unsaved = nullptr, bool* out_edit_started = nullptr);

private:
    char m_filter_buf[256] = {0};

    // Helper to draw a UE5-style component header.
    // Returns true if the section is open.
    // Sets *removed = true if the user clicks the remove button.
    bool drawComponentHeader(const char* label, bool can_remove, bool* removed,
                             ImVec4 accent_color = ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
};
