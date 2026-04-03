#include "ViewportOverlayPanel.hpp"
#include "EditorState.hpp"
#include "imgui.h"

void ViewportOverlayPanel::draw(const EditorState& state)
{
    if (!state.show_viewport_stats) return;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos(viewport->Pos.x + viewport->Size.x - 10.0f, viewport->Pos.y + 35.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);

    if (ImGui::Begin("##ViewportOverlay", nullptr, flags))
    {
        ImGui::Text("%.1f FPS (%.2f ms)", state.fps, state.delta_time * 1000.0f);
        ImGui::Separator();
        ImGui::Text("Entities: %zu total", state.total_entities);
        ImGui::Text("Visible:  %zu", state.visible_entities);
        ImGui::Text("Draw calls: %zu", state.draw_calls);
    }
    ImGui::End();
}
