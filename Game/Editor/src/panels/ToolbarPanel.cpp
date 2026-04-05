#include "ToolbarPanel.hpp"
#include "EditorState.hpp"
#include "NetworkPIESettings.hpp"
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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.48f, 0.80f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
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
                           IM_COL32(45, 42, 38, 200), 1.0f);
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
                           IM_COL32(45, 42, 38, 200), 1.0f);
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
                           IM_COL32(45, 42, 38, 200), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Centered Play / Pause / Stop / Eject ---
    {
        ImGuiStyle& s = ImGui::GetStyle();
        float play_group_width = 0.0f;
        switch (state.play_mode)
        {
        case PlayMode::Editing:
        {
            // Account for dynamic play label + dropdown arrow
            float arrow_w = ImGui::GetFrameHeight(); // ArrowButton is square
            if (state.network_pie.net_mode != PIENetMode::Standalone && state.network_pie.num_players > 1)
            {
                char buf[64];
                const char* ms = (state.network_pie.net_mode == PIENetMode::ListenServer) ? "Listen" : "Dedicated";
                snprintf(buf, sizeof(buf), "Play (%dP %s)", state.network_pie.num_players, ms);
                play_group_width = ImGui::CalcTextSize(buf).x + s.FramePadding.x * 2.0f + arrow_w;
            }
            else if (state.network_pie.net_mode != PIENetMode::Standalone)
            {
                char buf[64];
                const char* ms = (state.network_pie.net_mode == PIENetMode::ListenServer) ? "Listen" : "Dedicated";
                snprintf(buf, sizeof(buf), "Play (%s)", ms);
                play_group_width = ImGui::CalcTextSize(buf).x + s.FramePadding.x * 2.0f + arrow_w;
            }
            else
            {
                play_group_width = ImGui::CalcTextSize("Play").x + s.FramePadding.x * 2.0f + arrow_w;
            }
            break;
        }
        case PlayMode::Playing:
            play_group_width = ImGui::CalcTextSize("Pause").x + ImGui::CalcTextSize("Stop").x
                             + ImGui::CalcTextSize("Eject (F8)").x
                             + s.FramePadding.x * 6.0f + s.ItemSpacing.x * 2.0f;
            break;
        case PlayMode::Paused:
            play_group_width = ImGui::CalcTextSize("Resume").x + ImGui::CalcTextSize("Stop").x
                             + s.FramePadding.x * 4.0f + s.ItemSpacing.x;
            break;
        case PlayMode::Ejected:
            play_group_width = ImGui::CalcTextSize("Return (F8)").x + ImGui::CalcTextSize("Stop").x
                             + s.FramePadding.x * 4.0f + s.ItemSpacing.x;
            break;
        }
        float window_width = ImGui::GetWindowWidth();
        float center_x = (window_width - play_group_width) * 0.5f;
        float current_x = ImGui::GetCursorPosX();
        if (center_x > current_x)
            ImGui::SetCursorPosX(center_x);
    }

    switch (state.play_mode)
    {
    case PlayMode::Editing:
    {
        // Build play button label based on network PIE settings
        const char* play_label = "Play";
        char play_buf[64] = "Play";
        if (state.network_pie.net_mode != PIENetMode::Standalone && state.network_pie.num_players > 1)
        {
            const char* mode_short = (state.network_pie.net_mode == PIENetMode::ListenServer)
                                     ? "Listen" : "Dedicated";
            snprintf(play_buf, sizeof(play_buf), "Play (%dP %s)", state.network_pie.num_players, mode_short);
            play_label = play_buf;
        }
        else if (state.network_pie.net_mode != PIENetMode::Standalone)
        {
            const char* mode_short = (state.network_pie.net_mode == PIENetMode::ListenServer)
                                     ? "Listen" : "Dedicated";
            snprintf(play_buf, sizeof(play_buf), "Play (%s)", mode_short);
            play_label = play_buf;
        }

        // Green "Play" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.5f, 0.0f, 1.0f));
        if (ImGui::Button(play_label))
            if (callbacks.on_play) callbacks.on_play();
        ImGui::PopStyleColor(3);

        // Small dropdown arrow for network PIE settings
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.50f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.60f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.45f, 0.0f, 1.0f));
        if (ImGui::ArrowButton("##pie_settings", ImGuiDir_Down))
            ImGui::OpenPopup("PIENetSettings");
        ImGui::PopStyleColor(3);

        if (ImGui::BeginPopup("PIENetSettings"))
        {
            ImGui::Text("Multiplayer Settings");
            ImGui::Separator();

            // Net Mode combo
            const char* mode_names[] = { "Standalone", "Listen Server", "Dedicated Server" };
            int mode_idx = static_cast<int>(state.network_pie.net_mode);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Net Mode", &mode_idx, mode_names, 3))
                state.network_pie.net_mode = static_cast<PIENetMode>(mode_idx);

            // Only show extra settings for network modes
            bool is_network = (state.network_pie.net_mode != PIENetMode::Standalone);

            if (!is_network) ImGui::BeginDisabled();

            // Run Mode combo
            const char* run_names[] = { "In Editor", "Separate Windows" };
            int run_idx = static_cast<int>(state.network_pie.run_mode);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Run Mode", &run_idx, run_names, 2))
                state.network_pie.run_mode = static_cast<PIERunMode>(run_idx);

            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderInt("Players", &state.network_pie.num_players, 1, 4);

            int port_val = static_cast<int>(state.network_pie.server_port);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputInt("Port", &port_val, 1, 100))
            {
                if (port_val < 1024) port_val = 1024;
                if (port_val > 65535) port_val = 65535;
                state.network_pie.server_port = static_cast<uint16_t>(port_val);
            }

            if (!is_network) ImGui::EndDisabled();

            if (!has_game_module && is_network)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "No game module loaded.\nNetwork modes require a game DLL.");
            }

            ImGui::EndPopup();
        }

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

        // Blue "Eject" button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.4f, 0.8f, 1.0f));
        if (ImGui::Button("Eject (F8)"))
            if (callbacks.on_eject) callbacks.on_eject();
        ImGui::PopStyleColor(3);
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
                           IM_COL32(45, 42, 38, 200), 1.0f);
        ImGui::Dummy(ImVec2(10, h));
    }
    ImGui::SameLine();

    // --- Grid toggle ---
    ImGui::Checkbox("Grid", &state.show_grid);
}
