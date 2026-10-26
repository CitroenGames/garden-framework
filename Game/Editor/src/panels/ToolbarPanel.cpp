#include "ToolbarPanel.hpp"
#include "EditorState.hpp"
#include "NetworkPIESettings.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>

namespace
{
const char* launchModeLabel(PIELaunchMode mode)
{
    switch (mode)
    {
    case PIELaunchMode::SelectedViewport: return "Selected Viewport";
    case PIELaunchMode::NewEditorWindow: return "New Editor Window";
    case PIELaunchMode::VRPreview: return "VR Preview";
    case PIELaunchMode::StandaloneGame: return "Standalone Game";
    case PIELaunchMode::Simulate: return "Simulate";
    }
    return "Selected Viewport";
}

std::string buildPlayLabel(const EditorState& state)
{
    switch (state.pie_launch_mode)
    {
    case PIELaunchMode::SelectedViewport:
        if (state.network_pie.net_mode != PIENetMode::Standalone && state.network_pie.num_players > 1)
        {
            const char* mode_short = (state.network_pie.net_mode == PIENetMode::ListenServer)
                ? "Listen" : "Dedicated";
            return "Play " + std::to_string(state.network_pie.num_players) + "P " + mode_short;
        }
        if (state.network_pie.net_mode != PIENetMode::Standalone)
        {
            const char* mode_short = (state.network_pie.net_mode == PIENetMode::ListenServer)
                ? "Listen" : "Dedicated";
            return std::string("Play ") + mode_short;
        }
        return "Play";
    case PIELaunchMode::NewEditorWindow: return "Window";
    case PIELaunchMode::VRPreview: return "VR";
    case PIELaunchMode::StandaloneGame: return "Standalone";
    case PIELaunchMode::Simulate: return "Simulate";
    }
    return "Play";
}

void verticalSeparator(ImDrawList* draw_list)
{
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    draw_list->AddLine(ImVec2(p.x + 4, p.y + 2), ImVec2(p.x + 4, p.y + h - 2),
                       IM_COL32(42, 42, 42, 200), 1.0f);
    ImGui::Dummy(ImVec2(10, h));
    ImGui::SameLine();
}
} // namespace

void ToolbarPanel::draw(EditorState& state)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("Toolbar", nullptr, flags);
    drawContent(state);
    ImGui::End();
}

void ToolbarPanel::drawPlayMenuItems(EditorState& state)
{
    const bool active = state.isSimulationActive();

    ImGui::TextDisabled("Modes");
    auto launchItem = [&](const char* label, PIELaunchMode mode) {
        if (ImGui::MenuItem(label, nullptr, state.pie_launch_mode == mode, !active))
            state.pie_launch_mode = mode;
    };
    launchItem("Selected Viewport", PIELaunchMode::SelectedViewport);
    launchItem("New Editor Window", PIELaunchMode::NewEditorWindow);
    launchItem("VR Preview", PIELaunchMode::VRPreview);
    launchItem("Standalone Game", PIELaunchMode::StandaloneGame);
    launchItem("Simulate", PIELaunchMode::Simulate);

    ImGui::Separator();
    ImGui::TextDisabled("Quick Launch");
    if (ImGui::MenuItem(xr_launch_label.c_str(), nullptr, false, !active) && callbacks.on_quick_launch_vr)
        callbacks.on_quick_launch_vr();
    if (ImGui::MenuItem("Refresh OpenXR Runtime") && callbacks.on_refresh_openxr)
        callbacks.on_refresh_openxr();
    ImGui::TextDisabled("%s", xr_runtime_label.c_str());
    ImGui::TextDisabled("%s", xr_hmd_label.c_str());
    if (!xr_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s", xr_error.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Spawn Player At");
    if (ImGui::MenuItem("Current Camera Location", nullptr,
                        state.pie_spawn_location == PIESpawnLocation::CurrentCameraLocation, !active))
        state.pie_spawn_location = PIESpawnLocation::CurrentCameraLocation;
    if (ImGui::MenuItem("Default Player Start", nullptr,
                        state.pie_spawn_location == PIESpawnLocation::DefaultPlayerStart, !active))
        state.pie_spawn_location = PIESpawnLocation::DefaultPlayerStart;

    ImGui::Separator();
    ImGui::TextDisabled("Multiplayer Options");
    auto netItem = [&](const char* label, PIENetMode mode) {
        if (ImGui::MenuItem(label, nullptr, state.network_pie.net_mode == mode, !active))
            state.network_pie.net_mode = mode;
    };
    netItem("Standalone", PIENetMode::Standalone);
    netItem("Listen Server", PIENetMode::ListenServer);
    netItem("Dedicated Server", PIENetMode::DedicatedServer);

    const bool net = state.network_pie.net_mode != PIENetMode::Standalone;
    if (ImGui::MenuItem("Run In Editor", nullptr,
                        state.network_pie.run_mode == PIERunMode::InEditor, !active && net))
        state.network_pie.run_mode = PIERunMode::InEditor;
    if (ImGui::MenuItem("Separate Windows", nullptr,
                        state.network_pie.run_mode == PIERunMode::SeparateWindows, !active && net))
        state.network_pie.run_mode = PIERunMode::SeparateWindows;

    ImGui::BeginDisabled(active || !net);
    ImGui::SetNextItemWidth(120.0f);
    int players = state.network_pie.num_players;
    if (ImGui::DragInt("Players", &players, 0.1f, 1, 4))
        state.network_pie.num_players = std::clamp(players, 1, 4);
    ImGui::SetNextItemWidth(120.0f);
    int port = static_cast<int>(state.network_pie.server_port);
    if (ImGui::DragInt("Port", &port, 1.0f, 1024, 65535))
        state.network_pie.server_port = static_cast<uint16_t>(std::clamp(port, 1024, 65535));
    ImGui::EndDisabled();

    if (!has_game_module && net)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f),
                           "No game module loaded. Network modes require a game DLL.");
}

void ToolbarPanel::drawPlayControls(EditorState& state)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const bool active = state.isSimulationActive();
    const bool editing = !active;
    (void)editing;

    // --- Play cluster (formerly the RML shell header #play-group) ---
    if (!active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.58f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.40f, 0.13f, 1.0f));
        const std::string label = std::string(ICON_FA_PLAY "  ") + buildPlayLabel(state);
        if (ImGui::Button(label.c_str(), ImVec2(0, 32)) && callbacks.on_play)
            callbacks.on_play();
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, 2);
        if (ImGui::Button(ICON_FA_CHEVRON_DOWN "##play_options", ImVec2(28, 32)))
            ImGui::OpenPopup("PlayOptionsPopup");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Play options (%s)", launchModeLabel(state.pie_launch_mode));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", launchModeLabel(state.pie_launch_mode));

        if (ImGui::BeginPopup("PlayOptionsPopup"))
        {
            drawPlayMenuItems(state);
            ImGui::EndPopup();
        }
    }
    else
    {
        if (external_pie_active)
        {
            ImGui::TextDisabled("External");
            ImGui::SameLine();
        }
        if (state.play_mode == PlayMode::Paused)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.16f, 1.0f));
            if (ImGui::Button(ICON_FA_PLAY "  Resume", ImVec2(0, 32)) && callbacks.on_resume)
                callbacks.on_resume();
            ImGui::PopStyleColor();
        }
        else if (!external_pie_active && state.play_mode != PlayMode::Ejected)
        {
            if (ImGui::Button(ICON_FA_PAUSE "  Pause", ImVec2(0, 32)) && callbacks.on_pause)
                callbacks.on_pause();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button(ICON_FA_STOP "  Stop", ImVec2(0, 32)) && callbacks.on_stop)
            callbacks.on_stop();
        ImGui::PopStyleColor(2);
        if (!external_pie_active && state.play_mode == PlayMode::Playing)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_EJECT "  Eject F8", ImVec2(0, 32)) && callbacks.on_eject)
                callbacks.on_eject();
        }
        if (!external_pie_active && state.play_mode == PlayMode::Ejected)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT "  Return F8", ImVec2(0, 32)) && callbacks.on_return)
                callbacks.on_return();
        }
    }
}

void ToolbarPanel::drawContent(EditorState& state)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const bool active = state.isSimulationActive();

    // --- Transform mode buttons (disabled during play) ---
    bool editing = !active;

    auto modeButton = [&](const char* icon, const char* tooltip, EditorState::TransformMode mode)
    {
        bool is_active = (state.transform_mode == mode);
        if (is_active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.48f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.58f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.36f, 1.0f));
        }
        if (!editing)
            ImGui::BeginDisabled();
        if (ImGui::Button(icon, ImVec2(32, 32)))
            state.transform_mode = mode;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tooltip);
        if (is_active)
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
        if (is_active)
            ImGui::PopStyleColor(3);
    };

    modeButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (W)", EditorState::TransformMode::Translate);
    ImGui::SameLine();
    modeButton(ICON_FA_ROTATE, "Rotate (E)", EditorState::TransformMode::Rotate);
    ImGui::SameLine();
    modeButton(ICON_FA_EXPAND, "Scale (R)", EditorState::TransformMode::Scale);

    verticalSeparator(draw_list);

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

    verticalSeparator(draw_list);

    // --- Local/World space toggle ---
    if (!editing)
        ImGui::BeginDisabled();
    {
        bool is_local = (state.gizmo_space == EditorState::GizmoSpace::Local);
        const char* icon = is_local ? ICON_FA_CROSSHAIRS : ICON_FA_GLOBE;
        const char* tooltip = is_local ? "Local Space (click to switch to World)" : "World Space (click to switch to Local)";
        if (ImGui::Button(icon, ImVec2(32, 32)))
            state.gizmo_space = is_local ? EditorState::GizmoSpace::World : EditorState::GizmoSpace::Local;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tooltip);
    }
    if (!editing)
        ImGui::EndDisabled();

    verticalSeparator(draw_list);

    // --- Grid toggle ---
    ImGui::Checkbox(ICON_FA_BORDER_ALL " Grid", &state.show_grid);
}
