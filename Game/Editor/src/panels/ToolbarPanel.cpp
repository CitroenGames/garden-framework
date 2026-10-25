#include "ToolbarPanel.hpp"
#include "EditorState.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include "imgui_internal.h"

void ToolbarPanel::draw(EditorState& state)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("Toolbar", nullptr, flags);
    drawContent(state);
    ImGui::End();
}

void ToolbarPanel::drawContent(EditorState& state)
{
    // NOTE: Play/Pause/Stop/Eject live solely in the RML shell header
    // (assets/Editor/RML/EditorShell.rml #play-group). This strip holds only
    // viewport transform tools so there is exactly one Play button.
    // Play callbacks on ToolbarCallbacks are retained for API compatibility
    // but are no longer driven from here.
    (void)callbacks;
    (void)has_game_module;

    // --- Transform mode buttons (disabled during play) ---
    bool editing = !state.isSimulationActive();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    auto modeButton = [&](const char* icon, const char* tooltip, EditorState::TransformMode mode)
    {
        bool active = (state.transform_mode == mode);
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.48f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.58f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.36f, 1.0f));
        }

        if (!editing)
            ImGui::BeginDisabled();

        if (ImGui::Button(icon, ImVec2(32, 32)))
            state.transform_mode = mode;

        // Tooltip
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tooltip);

        // Active underline indicator (logo teal)
        if (active)
        {
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            draw_list->AddRectFilled(
                ImVec2(btn_min.x, btn_max.y - 2.0f),
                ImVec2(btn_max.x, btn_max.y),
                IM_COL32(42, 161, 152, 255));
        }

        if (!editing)
            ImGui::EndDisabled();

        if (active)
            ImGui::PopStyleColor(3);
    };

    modeButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (W)", EditorState::TransformMode::Translate);
    ImGui::SameLine();
    modeButton(ICON_FA_ROTATE, "Rotate (E)", EditorState::TransformMode::Rotate);
    ImGui::SameLine();
    modeButton(ICON_FA_EXPAND, "Scale (R)", EditorState::TransformMode::Scale);

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(42, 42, 42, 200), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Snap ---
    if (!editing)
        ImGui::BeginDisabled();

    ImGui::Checkbox(ICON_FA_MAGNET " Snap", &state.snap_enabled);
    if (state.snap_enabled)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        switch (state.transform_mode)
        {
        case EditorState::TransformMode::Translate:
            ImGui::DragFloat("##snap_val", &state.snap_translate, 0.1f, 0.01f, 100.0f, "%.2f");
            break;
        case EditorState::TransformMode::Rotate:
            ImGui::DragFloat("##snap_val", &state.snap_rotate, 1.0f, 1.0f, 180.0f, "%.0f");
            break;
        case EditorState::TransformMode::Scale:
            ImGui::DragFloat("##snap_val", &state.snap_scale, 0.01f, 0.01f, 10.0f, "%.2f");
            break;
        }
    }

    if (!editing)
        ImGui::EndDisabled();

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(42, 42, 42, 200), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Local/World space toggle ---
    if (!editing)
        ImGui::BeginDisabled();

    {
        bool is_local = (state.gizmo_space == EditorState::GizmoSpace::Local);
        const char* icon = is_local ? ICON_FA_CROSSHAIRS : ICON_FA_GLOBE;
        const char* tooltip = is_local ? "Local Space (click to switch to World)" : "World Space (click to switch to Local)";
        if (ImGui::Button(icon, ImVec2(32, 32)))
        {
            state.gizmo_space = is_local
                ? EditorState::GizmoSpace::World
                : EditorState::GizmoSpace::Local;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tooltip);
    }

    if (!editing)
        ImGui::EndDisabled();

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(42, 42, 42, 200), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Grid toggle ---
    ImGui::Checkbox(ICON_FA_BORDER_ALL " Grid", &state.show_grid);
}
