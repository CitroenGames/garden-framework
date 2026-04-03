#include "StatusBarPanel.hpp"
#include "EditorState.hpp"
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
                           IM_COL32(50, 50, 50, 255), 1.0f);

        // Left side: stats
        ImGui::Text("FPS: %.0f | Entities: %zu | Visible: %zu | Draws: %zu",
            state.fps, state.total_entities, state.visible_entities, state.draw_calls);

        // Center: transform mode or play state
        float center_x = viewport->Size.x * 0.5f;

        if (state.isSimulationActive())
        {
            const char* mode_label = "PLAYING";
            ImVec4 color(0.2f, 0.8f, 0.2f, 1.0f); // green
            ImU32 dot_color = IM_COL32(51, 204, 51, 255);

            if (state.play_mode == PlayMode::Paused)
            {
                mode_label = "PAUSED";
                color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // yellow
                dot_color = IM_COL32(255, 204, 0, 255);
            }
            else if (state.play_mode == PlayMode::Ejected)
            {
                mode_label = "EJECTED";
                color = ImVec4(0.3f, 0.55f, 1.0f, 1.0f); // blue
                dot_color = IM_COL32(77, 140, 255, 255);
            }

            float text_width = ImGui::CalcTextSize(mode_label).x;
            float total_width = text_width + 14.0f; // dot + spacing
            ImGui::SameLine(center_x - total_width * 0.5f);

            // Colored indicator dot
            ImVec2 dot_pos = ImGui::GetCursorScreenPos();
            float line_h = ImGui::GetTextLineHeight();
            draw_list->AddCircleFilled(ImVec2(dot_pos.x + 4.0f, dot_pos.y + line_h * 0.5f), 4.0f, dot_color);
            ImGui::Dummy(ImVec2(14.0f, line_h));
            ImGui::SameLine();

            ImGui::TextColored(color, "%s", mode_label);
        }
        else
        {
            const char* mode_str = "Translate";
            switch (state.transform_mode)
            {
            case EditorState::TransformMode::Rotate: mode_str = "Rotate"; break;
            case EditorState::TransformMode::Scale:  mode_str = "Scale";  break;
            default: break;
            }
            float text_width = ImGui::CalcTextSize(mode_str).x;
            ImGui::SameLine(center_x - text_width * 0.5f);
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
