#include "ViewportPanel.hpp"
#include "ToolbarPanel.hpp"
#include "imgui_internal.h"

void ViewportPanel::draw(ImTextureID scene_texture, EditorState& state)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    // --- Toolbar strip (inside the viewport window) ---
    if (toolbar && show_toolbar && *show_toolbar)
    {
        float toolbar_height = ImGui::GetFrameHeight() + 8.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
        ImGui::BeginChild("##ToolbarStrip", ImVec2(0, toolbar_height), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        toolbar->drawContent(state);

        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Subtle separator line below toolbar
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
                    IM_COL32(60, 60, 60, 200), 1.0f);
    }

    // --- Scene image (fills remaining space) ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int new_w = static_cast<int>(avail.x);
    int new_h = static_cast<int>(avail.y);

    if (new_w > 0 && new_h > 0)
    {
        width = new_w;
        height = new_h;
    }

    PlayMode play_mode = state.play_mode;

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
