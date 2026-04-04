#include "ToolbarPanel.hpp"
#include "EditorState.hpp"
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
    // --- Transform mode buttons (disabled during play) ---
    bool editing = !state.isSimulationActive();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    auto modeButton = [&](const char* label, const char* tooltip, EditorState::TransformMode mode)
    {
        bool active = (state.transform_mode == mode);
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.4f, 0.8f, 1.0f));
        }

        if (!editing)
            ImGui::BeginDisabled();

        if (ImGui::Button(label, ImVec2(28, 28)))
            state.transform_mode = mode;

        // Tooltip
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tooltip);

        // Active underline indicator
        if (active)
        {
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            draw_list->AddRectFilled(
                ImVec2(btn_min.x, btn_max.y - 2.0f),
                ImVec2(btn_max.x, btn_max.y),
                IM_COL32(77, 140, 255, 255));
        }

        if (!editing)
            ImGui::EndDisabled();

        if (active)
            ImGui::PopStyleColor(3);
    };

    modeButton("W", "Translate (W)", EditorState::TransformMode::Translate);
    ImGui::SameLine();
    modeButton("E", "Rotate (E)", EditorState::TransformMode::Rotate);
    ImGui::SameLine();
    modeButton("R", "Scale (R)", EditorState::TransformMode::Scale);

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(60, 60, 60, 255), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Snap ---
    if (!editing)
        ImGui::BeginDisabled();

    ImGui::Checkbox("Snap", &state.snap_enabled);
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
                           IM_COL32(60, 60, 60, 255), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Local/World space toggle ---
    if (!editing)
        ImGui::BeginDisabled();

    {
        bool is_local = (state.gizmo_space == EditorState::GizmoSpace::Local);
        const char* label = is_local ? "Local" : "World";
        if (ImGui::Button(label, ImVec2(50, 28)))
        {
            state.gizmo_space = is_local
                ? EditorState::GizmoSpace::World
                : EditorState::GizmoSpace::Local;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Toggle Local/World transform space");
    }

    if (!editing)
        ImGui::EndDisabled();

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(60, 60, 60, 255), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Play / Pause / Stop / Eject ---
    switch (state.play_mode)
    {
    case PlayMode::Editing:
    {
        // Green "Play" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.5f, 0.0f, 1.0f));
        if (ImGui::Button("Play"))
            if (callbacks.on_play) callbacks.on_play();
        ImGui::PopStyleColor(3);
        break;
    }
    case PlayMode::Playing:
    {
        // Yellow "Pause" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.7f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.8f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button("Pause"))
            if (callbacks.on_pause) callbacks.on_pause();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // Red "Stop" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::Button("Stop"))
            if (callbacks.on_stop) callbacks.on_stop();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // "Eject" button
        if (ImGui::Button("Eject (F8)"))
            if (callbacks.on_eject) callbacks.on_eject();
        break;
    }
    case PlayMode::Paused:
    {
        // Green "Resume" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.5f, 0.0f, 1.0f));
        if (ImGui::Button("Resume"))
            if (callbacks.on_resume) callbacks.on_resume();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // Red "Stop" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::Button("Stop"))
            if (callbacks.on_stop) callbacks.on_stop();
        ImGui::PopStyleColor(3);
        break;
    }
    case PlayMode::Ejected:
    {
        // Blue "Return" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.4f, 0.8f, 1.0f));
        if (ImGui::Button("Return (F8)"))
            if (callbacks.on_return) callbacks.on_return();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // Red "Stop" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::Button("Stop"))
            if (callbacks.on_stop) callbacks.on_stop();
        ImGui::PopStyleColor(3);
        break;
    }
    }

    // Custom vertical separator
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                           IM_COL32(60, 60, 60, 255), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Grid toggle ---
    ImGui::Checkbox("Grid", &state.show_grid);
}
