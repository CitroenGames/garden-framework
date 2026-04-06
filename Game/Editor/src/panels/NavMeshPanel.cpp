#include "NavMeshPanel.hpp"
#include "Navigation/NavMeshGenerator.hpp"
#include "Navigation/NavMeshPathfinder.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "Navigation/NavMeshDebugDraw.hpp"
#include "Debug/DebugDraw.hpp"
#include "imgui.h"

static void drawSectionHeader(const char* label, ImVec4 accent_color = ImVec4(0.30f, 0.55f, 0.85f, 1.0f))
{
    ImGui::Spacing();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float avail_w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;

    draw_list->AddRectFilled(pos, ImVec2(pos.x + avail_w, pos.y + h),
                             IM_COL32(30, 30, 30, 255), 3.0f);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    ImGui::Indent(6.0f);
    ImGui::TextColored(accent_color, "%s", label);
    ImGui::Unindent(6.0f);

    ImVec2 line_start(pos.x, pos.y + h);
    ImU32 line_col = IM_COL32((int)(accent_color.x * 255), (int)(accent_color.y * 255),
                               (int)(accent_color.z * 255), 128);
    draw_list->AddLine(line_start, ImVec2(line_start.x + avail_w, line_start.y), line_col, 1.0f);

    ImGui::Spacing();
}

void NavMeshPanel::draw()
{
    ImGui::Begin("NavMesh");

    if (!registry)
    {
        ImGui::TextDisabled("No world registry.");
        ImGui::End();
        return;
    }

    // ── Agent Settings ──────────────────────────────────────────────────
    drawSectionHeader("Agent Settings", ImVec4(0.3f, 0.8f, 0.5f, 1.0f));

    ImGui::DragFloat("Max Slope Angle", &m_config.max_slope_angle, 0.5f, 0.0f, 90.0f, "%.1f deg");
    ImGui::DragFloat("Agent Height",    &m_config.agent_height, 0.05f, 0.1f, 10.0f, "%.2f");
    ImGui::DragFloat("Agent Radius",    &m_config.agent_radius, 0.01f, 0.01f, 5.0f, "%.2f");
    ImGui::DragFloat("Max Climb",       &m_config.max_climb, 0.01f, 0.0f, 5.0f, "%.2f");

    // ── Advanced Recast Settings ────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced (Recast)"))
    {
        m_show_advanced = true;
        ImGui::DragFloat("Cell Size",        &m_config.cell_size, 0.01f, 0.05f, 2.0f, "%.2f");
        ImGui::DragFloat("Cell Height",      &m_config.cell_height, 0.01f, 0.05f, 2.0f, "%.2f");
        ImGui::DragFloat("Max Edge Length",   &m_config.max_edge_len, 0.5f, 0.0f, 50.0f, "%.1f");
        ImGui::DragFloat("Max Edge Error",    &m_config.max_edge_error, 0.1f, 0.0f, 10.0f, "%.1f");
        ImGui::DragInt("Min Region Area",     &m_config.min_region_area, 1, 0, 1000);
        ImGui::DragInt("Merge Region Area",   &m_config.merge_region_area, 1, 0, 1000);
        ImGui::DragInt("Max Verts/Poly",      &m_config.max_verts_per_poly, 1, 3, 6);
        ImGui::DragFloat("Detail Sample Dist",      &m_config.detail_sample_dist, 0.5f, 0.0f, 50.0f, "%.1f");
        ImGui::DragFloat("Detail Sample Max Error",  &m_config.detail_sample_max_error, 0.1f, 0.0f, 10.0f, "%.1f");
    }

    ImGui::Spacing();
    if (ImGui::Button("Generate NavMesh", ImVec2(-1.0f, 0.0f)))
    {
        Navigation::NavMeshGenerator::GenerationStats stats;
        navmesh = Navigation::NavMeshGenerator::generate(*registry, m_config, &stats);
        m_total_source_tris = stats.source_triangles;
        m_total_polys = stats.total_polys;
        m_generation_time_ms = stats.time_ms;
        m_test_path = Navigation::NavPath{};
    }

    // ── Stats ────────────────────────────────────────────────────────────
    drawSectionHeader("Stats", ImVec4(0.3f, 0.55f, 0.85f, 1.0f));

    ImGui::Text("Source triangles: %d", m_total_source_tris);
    ImGui::Text("NavMesh polygons: %d", m_total_polys);
    ImGui::Text("Generation time:  %.1f ms", m_generation_time_ms);

    if (navmesh.valid)
    {
        ImGui::Text("Debug polys:      %d", static_cast<int>(navmesh.debug_polys.size()));
    }

    // ── Visualization ────────────────────────────────────────────────────
    drawSectionHeader("Visualization", ImVec4(1.0f, 0.85f, 0.3f, 1.0f));

    ImGui::Checkbox("Show NavMesh", &m_show_visualization);

    if (m_show_visualization)
    {
        ImGui::Checkbox("Wireframe",  &m_debug_config.show_wireframe);
        if (m_debug_config.show_wireframe)
        {
            ImGui::SameLine();
            ImGui::ColorEdit3("##wire_col", &m_debug_config.wireframe_color.x,
                              ImGuiColorEditFlags_NoInputs);
        }

        ImGui::Checkbox("Normals", &m_debug_config.show_normals);
        if (m_debug_config.show_normals)
        {
            ImGui::SameLine();
            ImGui::ColorEdit3("##norm_col", &m_debug_config.normal_color.x,
                              ImGuiColorEditFlags_NoInputs);
        }

        ImGui::Checkbox("Adjacency", &m_debug_config.show_adjacency);
        if (m_debug_config.show_adjacency)
        {
            ImGui::SameLine();
            ImGui::ColorEdit3("##adj_col", &m_debug_config.adjacency_color.x,
                              ImGuiColorEditFlags_NoInputs);
        }

        ImGui::DragFloat("Y Offset", &m_debug_config.wireframe_y_offset, 0.005f, 0.0f, 1.0f, "%.3f");
    }

    // ── Serialization ────────────────────────────────────────────────────
    drawSectionHeader("Serialization", ImVec4(1.0f, 0.5f, 0.2f, 1.0f));

    ImGui::InputText("File Path", m_filepath_buf, sizeof(m_filepath_buf));

    if (ImGui::Button("Save") && navmesh.valid)
    {
        if (Navigation::NavMeshSerializer::save(navmesh, m_filepath_buf))
            ImGui::SetTooltip("Saved!");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        if (Navigation::NavMeshSerializer::load(navmesh, m_filepath_buf))
        {
            m_config = navmesh.config;
            m_total_polys = navmesh.total_polys;
            m_total_source_tris = 0;
            m_generation_time_ms = 0.0f;
            m_test_path = Navigation::NavPath{};

            // Re-extract debug polys after load
            // (They aren't serialized - regenerate from Detour data)
            Navigation::NavMeshGenerator::extractDebugPolys(navmesh);
        }
    }

    // ── Path Testing ─────────────────────────────────────────────────────
    drawSectionHeader("Path Testing", ImVec4(0.7f, 0.3f, 0.85f, 1.0f));

    ImGui::Checkbox("Enable Path Testing", &m_path_test_mode);

    if (m_path_test_mode && navmesh.valid)
    {
        ImGui::DragFloat3("Start", &m_path_start.x, 0.1f);
        ImGui::DragFloat3("Goal",  &m_path_goal.x, 0.1f);

        if (ImGui::Button("Find Path", ImVec2(-1.0f, 0.0f)))
        {
            m_test_path = Navigation::NavMeshPathfinder::findPath(navmesh, m_path_start, m_path_goal);
        }

        if (m_test_path.valid)
            ImGui::Text("Waypoints: %d", static_cast<int>(m_test_path.waypoints.size()));
        else
            ImGui::TextDisabled("No path found");
    }

    ImGui::End();
}

void NavMeshPanel::drawDebugVisualization()
{
    if (!m_show_visualization || !navmesh.valid)
        return;

    Navigation::NavMeshDebugDraw::draw(navmesh, m_debug_config);

    if (m_path_test_mode && m_test_path.valid)
    {
        Navigation::NavMeshDebugDraw::drawPath(m_test_path, m_debug_config.path_color);

        // Draw start/goal markers
        auto& dd = DebugDraw::get();
        dd.drawSphere(m_path_start, 0.15f, glm::vec3(0.0f, 1.0f, 0.0f));
        dd.drawSphere(m_path_goal, 0.15f, glm::vec3(1.0f, 0.0f, 0.0f));
    }
}
