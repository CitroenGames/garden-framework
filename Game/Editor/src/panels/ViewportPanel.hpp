#pragma once

#include "imgui.h"
#include "EditorState.hpp"
#include <entt/entt.hpp>

class ToolbarPanel;
class camera;
class IRenderAPI;
class SceneBVH;

struct GizmoResult
{
    bool transform_changed = false;
    bool drag_started = false;
};

class ViewportPanel
{
public:
    // Current viewport content size (used by EditorApp to resize render target)
    int width = 800;
    int height = 600;
    bool is_hovered = false;

    // Set by EditorApp during initialization
    ToolbarPanel* toolbar = nullptr;
    bool* show_toolbar = nullptr;

    GizmoResult draw(ImTextureID scene_texture, EditorState& state,
                     entt::registry& registry, entt::entity& selected,
                     camera& cam, IRenderAPI* render_api, SceneBVH& bvh);

private:
    // Gizmo drag-start detection
    bool m_was_using = false;

    // Cached image rect for gizmo overlay
    ImVec2 m_image_min = {0, 0};
    ImVec2 m_image_max = {0, 0};

    void drawGizmo(EditorState& state, entt::registry& registry,
                   entt::entity selected, camera& cam, IRenderAPI* render_api,
                   GizmoResult& result);

    void handlePicking(bool image_clicked, entt::registry& registry,
                       entt::entity& selected, IRenderAPI* render_api, SceneBVH& bvh);
};
