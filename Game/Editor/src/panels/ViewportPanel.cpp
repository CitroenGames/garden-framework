#include "ViewportPanel.hpp"

void ViewportPanel::draw(ImTextureID scene_texture, PlayMode play_mode)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int new_w = static_cast<int>(avail.x);
    int new_h = static_cast<int>(avail.y);

    if (new_w > 0 && new_h > 0)
    {
        width = new_w;
        height = new_h;
    }

    if (scene_texture)
    {
        // UV flipped vertically for OpenGL (0,1 -> 1,0)
        ImGui::Image(scene_texture, avail, ImVec2(0, 1), ImVec2(1, 0));

        // Draw colored border when simulation is active
        if (play_mode != PlayMode::Editing)
        {
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();

            ImU32 border_color;
            switch (play_mode)
            {
            case PlayMode::Playing:
                border_color = IM_COL32(50, 200, 50, 200);   // green
                break;
            case PlayMode::Paused:
                border_color = IM_COL32(255, 200, 0, 200);   // yellow
                break;
            case PlayMode::Ejected:
                border_color = IM_COL32(80, 140, 255, 200);  // blue
                break;
            default:
                border_color = IM_COL32(255, 255, 255, 100);
                break;
            }

            ImGui::GetWindowDrawList()->AddRect(min, max, border_color, 0.0f, 0, 3.0f);
        }
    }
    else
    {
        ImGui::TextDisabled("No viewport texture available");
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
