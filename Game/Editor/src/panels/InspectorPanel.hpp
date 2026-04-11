#pragma once

#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <filesystem>
#include <cstdint>
#include <glm/glm.hpp>
#include "imgui.h"

class ReflectionRegistry;
struct ComponentDescriptor;

class InspectorPanel
{
public:
    // entity -> original mesh file path (ECS holds GPU ptr, not a string)
    std::unordered_map<entt::entity, std::string> mesh_path_cache;

    // Set by EditorApp each frame for LOD debug display
    glm::vec3 debug_cam_pos{0.0f};
    glm::mat4 debug_projection{1.0f};

    // Reflection registry for data-driven component rendering
    ReflectionRegistry* reflection = nullptr;

    // Callback: load a mesh file for an entity (wired by EditorApp)
    std::function<void(entt::entity, const std::string&)> on_browse_mesh;

    // Root path for asset scanning
    std::filesystem::path asset_base_path = "assets";

    // Draw the inspector for the selected entity.
    // Returns true if any transform was modified (so caller can mark BVH dirty).
    // out_unsaved: set to true when any property is modified.
    // out_edit_started: set to true when a drag/edit operation begins (for undo snapshots).
    bool draw(entt::registry& registry, entt::entity selected,
              bool* out_unsaved = nullptr, bool* out_edit_started = nullptr,
              bool* p_open = nullptr);

private:
    char m_filter_buf[256] = {0};

    // Component clipboard
    uint32_t m_clipboard_type_id = 0;
    std::vector<uint8_t> m_clipboard_data;
    bool m_has_clipboard = false;

    // Mesh asset picker state
    bool m_mesh_picker_open = false;
    entt::entity m_mesh_picker_entity{entt::null};
    char m_mesh_picker_search[256] = {0};
    std::vector<std::string> m_mesh_file_cache;

    // Helper to draw a UE5-style component header.
    // Returns true if the section is open.
    // Sets *removed = true if the user clicks the remove button.
    bool drawComponentHeader(const char* label, bool can_remove, bool* removed,
                             ImVec4 accent_color = ImVec4(0.30f, 0.55f, 0.85f, 1.0f));

    // Mesh picker helpers
    void drawMeshPickerPopup();
    void refreshMeshFileCache();

    // Component clipboard operations
    void copyComponentToClipboard(const ComponentDescriptor& desc, void* src);
    void pasteComponentFromClipboard(const ComponentDescriptor& desc, void* dest);
    void resetComponentToDefaults(const ComponentDescriptor& desc, void* dest);
};
