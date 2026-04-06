#pragma once

#include <string>
#include <entt/entt.hpp>
#include "panels/PreviewOrbitCamera.hpp"

struct PrefabEditorInstance
{
    // Identity
    std::string prefab_path;
    std::string prefab_name;
    int id = 0;

    // Isolated ECS (not the main scene)
    entt::registry registry;
    entt::entity entity = entt::null;

    // 3D Preview
    int viewport_id = -1;           // from IRenderAPI::createPIEViewport()
    int viewport_width = 400;
    int viewport_height = 300;
    PreviewOrbitCamera orbit;

    // Mesh tracking (for serialization back to .prefab)
    std::string mesh_path;
    std::string collider_mesh_path;

    // UI state
    uint32_t selected_component_type = 0;
    bool has_selection = false;
    bool unsaved_changes = false;
    bool wants_close = false;
    bool show_save_prompt = false;
    bool is_open = true;

    // Unique ImGui window ID: "Prefab: <name>##Prefab<id>"
    std::string window_id;
};
