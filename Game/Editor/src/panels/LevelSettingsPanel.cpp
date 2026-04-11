#include "LevelSettingsPanel.hpp"
#include "PanelUtils.hpp"
#include "imgui.h"
#include <glm/glm.hpp>
#include <cstring>

static void drawSectionHeader(const char* label, ImVec4 accent_color = ImVec4(0.30f, 0.55f, 0.85f, 1.0f))
{
    ImGui::Spacing();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float avail_w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;

    // Background bar
    draw_list->AddRectFilled(pos, ImVec2(pos.x + avail_w, pos.y + h),
                             IM_COL32(30, 30, 30, 255), 3.0f);

    // Accent-colored text
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    ImGui::Indent(6.0f);
    ImGui::TextColored(accent_color, "%s", label);
    ImGui::Unindent(6.0f);

    // Thin accent line below
    ImVec2 line_start(pos.x, pos.y + h);
    ImU32 line_col = IM_COL32((int)(accent_color.x * 255), (int)(accent_color.y * 255),
                               (int)(accent_color.z * 255), 128);
    draw_list->AddLine(line_start, ImVec2(line_start.x + avail_w, line_start.y), line_col, 1.0f);

    ImGui::Spacing();
}

void LevelSettingsPanel::draw(bool* p_open)
{
    ImGui::Begin("Level Settings", p_open);
    PanelMaximizeButton();

    if (!metadata)
    {
        ImGui::TextDisabled("No level loaded.");
        ImGui::End();
        return;
    }

    drawSectionHeader("Level Info", ImVec4(0.86f, 0.86f, 0.86f, 1.0f));

    // Level name
    char name_buf[256];
    std::strncpy(name_buf, metadata->level_name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    if (ImGui::InputText("Level Name", name_buf, sizeof(name_buf)))
        metadata->level_name = name_buf;

    char author_buf[256];
    std::strncpy(author_buf, metadata->author.c_str(), sizeof(author_buf) - 1);
    author_buf[sizeof(author_buf) - 1] = '\0';
    if (ImGui::InputText("Author", author_buf, sizeof(author_buf)))
        metadata->author = author_buf;

    drawSectionHeader("Lighting", ImVec4(1.0f, 0.85f, 0.3f, 1.0f));

    ImGui::ColorEdit3("Ambient Light", &metadata->ambient_light.x);
    ImGui::ColorEdit3("Diffuse Light", &metadata->diffuse_light.x);
    ImGui::DragFloat3("Light Direction", &metadata->light_direction.x, 0.01f, -1.0f, 1.0f);
    if (ImGui::Button("Normalize Direction"))
    {
        float len = glm::length(metadata->light_direction);
        if (len > 0.0001f)
            metadata->light_direction /= len;
    }

    drawSectionHeader("World", ImVec4(0.3f, 0.8f, 0.5f, 1.0f));

    ImGui::DragFloat3("Gravity", &metadata->gravity.x, 0.01f);
    ImGui::DragFloat("Fixed Delta", &metadata->fixed_delta, 0.0001f, 0.001f, 1.0f, "%.4f");

    ImGui::End();
}
