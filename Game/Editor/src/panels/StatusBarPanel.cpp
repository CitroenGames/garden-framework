#include "StatusBarPanel.hpp"
#include "EditorState.hpp"
#include "NetworkPIESettings.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"

void StatusBarPanel::draw(const EditorState& state)
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));

    if (ImGui::Begin("##StatusBar", nullptr, flags))
    {
        // Top border line
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 win_pos = ImGui::GetWindowPos();
        draw_list->AddLine(win_pos, ImVec2(win_pos.x + viewport->Size.x, win_pos.y),
                           IM_COL32(42, 42, 42, 255), 1.0f);

        // Left side: stats
        ImGui::Text("FPS: %.0f | Entities: %zu | " ICON_FA_EYE " %zu | Draws: %zu",
            state.fps, state.total_entities, state.visible_entities, state.draw_calls);

        // Center: transform mode or play state
        float center_x = viewport->Size.x * 0.5f;

        if (state.isSimulationActive())
        {
            const char* mode_icon = ICON_FA_PLAY;
            const char* mode_label = "PLAYING";
            ImVec4 color(0.2f, 0.8f, 0.2f, 1.0f);

            if (state.play_mode == PlayMode::Paused)
            {
                mode_icon = ICON_FA_PAUSE;
                mode_label = "PAUSED";
                color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
            }
            else if (state.play_mode == PlayMode::Ejected)
            {
                mode_icon = ICON_FA_EJECT;
                mode_label = "EJECTED";
                color = ImVec4(0.3f, 0.55f, 1.0f, 1.0f);
            }

            // Calculate total width: icon + space + label
            float icon_width = ImGui::CalcTextSize(mode_icon).x;
            float text_width = ImGui::CalcTextSize(mode_label).x;
            float total_width = icon_width + 6.0f + text_width;
            ImGui::SameLine(center_x - total_width * 0.5f);

            ImGui::TextColored(color, "%s", mode_icon);
            ImGui::SameLine(0, 6.0f);
            ImGui::TextColored(color, "%s", mode_label);

            // Show network PIE info
            if (network_pie_active)
            {
                ImGui::SameLine();
                const char* net_mode = (state.network_pie.net_mode == PIENetMode::ListenServer)
                                       ? "Listen" : "Dedicated";
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "| " ICON_FA_GLOBE " %s :%d",
                    net_mode, state.network_pie.server_port);
                if (spawned_processes > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "| %d ext. client%s",
                        spawned_processes, spawned_processes > 1 ? "s" : "");
                }
            }
        }
        else
        {
            const char* mode_icon = ICON_FA_UP_DOWN_LEFT_RIGHT;
            const char* mode_str = "Translate";
            switch (state.transform_mode)
            {
            case EditorState::TransformMode::Rotate:
                mode_icon = ICON_FA_ROTATE;
                mode_str = "Rotate";
                break;
            case EditorState::TransformMode::Scale:
                mode_icon = ICON_FA_EXPAND;
                mode_str = "Scale";
                break;
            default: break;
            }
            float icon_width = ImGui::CalcTextSize(mode_icon).x;
            float text_width = ImGui::CalcTextSize(mode_str).x;
            float total_width = icon_width + 6.0f + text_width;
            ImGui::SameLine(center_x - total_width * 0.5f);
            ImGui::TextDisabled("%s", mode_icon);
            ImGui::SameLine(0, 6.0f);
            ImGui::Text("%s", mode_str);
        }

        // Right side: file path
        const char* path_str = state.current_save_path.empty() ? "(unsaved)" : state.current_save_path.c_str();
        float path_width = ImGui::CalcTextSize(path_str).x;
        ImGui::SameLine(viewport->Size.x - path_width - 15.0f);
        if (state.current_save_path.empty())
            ImGui::TextDisabled("%s", path_str);
        else
            ImGui::Text("%s", path_str);
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
