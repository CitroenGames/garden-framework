#include "PhysicsDebugPanel.hpp"
#include "Components/Components.hpp"
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

void PhysicsDebugPanel::draw()
{
    ImGui::Begin("Physics Debug");

    if (!registry)
    {
        ImGui::TextDisabled("No world registry.");
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enable Physics Debug", &m_config.enabled);

    if (!m_config.enabled)
    {
        ImGui::End();
        return;
    }

    // -- Collider AABBs --
    drawSectionHeader("Collider AABBs", ImVec4(0.0f, 0.8f, 0.2f, 1.0f));
    ImGui::Checkbox("Show Collider AABBs", &m_config.show_collider_aabb);
    if (m_config.show_collider_aabb)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("##collider_col", &m_config.collider_color.x,
                          ImGuiColorEditFlags_NoInputs);
    }

    // -- Player Capsules --
    drawSectionHeader("Player Capsules", ImVec4(0.0f, 0.8f, 0.8f, 1.0f));
    ImGui::Checkbox("Show Player Capsules", &m_config.show_player_capsules);
    if (m_config.show_player_capsules)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("##capsule_col", &m_config.capsule_color.x,
                          ImGuiColorEditFlags_NoInputs);
    }

    // -- Velocity Vectors --
    drawSectionHeader("Velocity Vectors", ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
    ImGui::Checkbox("Show Velocity", &m_config.show_velocity);
    if (m_config.show_velocity)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("##vel_col", &m_config.velocity_color.x,
                          ImGuiColorEditFlags_NoInputs);
        ImGui::DragFloat("Velocity Scale", &m_config.velocity_scale, 0.05f, 0.1f, 10.0f, "%.2f");
    }

    // -- RigidBody AABBs --
    drawSectionHeader("RigidBody AABBs", ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
    ImGui::Checkbox("Show RigidBody AABBs", &m_config.show_rigidbody_aabb);
    if (m_config.show_rigidbody_aabb)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("##rb_col", &m_config.rigidbody_color.x,
                          ImGuiColorEditFlags_NoInputs);
    }

    ImGui::End();
}

void PhysicsDebugPanel::drawDebugVisualization()
{
    if (!m_config.enabled || !registry)
        return;

    auto& dd = DebugDraw::get();

    // 1. Collider AABBs (skip player entities — they get capsules instead)
    if (m_config.show_collider_aabb)
    {
        auto view = registry->view<ColliderComponent, TransformComponent>();
        for (auto entity : view)
        {
            if (registry->all_of<PlayerComponent>(entity))
                continue;

            auto& collider  = view.get<ColliderComponent>(entity);
            auto& transform = view.get<TransformComponent>(entity);

            if (!collider.is_mesh_valid())
                continue;

            glm::vec3 mesh_min, mesh_max;
            collider.get_mesh()->getAABB(mesh_min, mesh_max);

            glm::vec3 world_min = mesh_min + transform.position;
            glm::vec3 world_max = mesh_max + transform.position;

            dd.drawAABB(world_min, world_max, m_config.collider_color);
        }
    }

    // 2. Player capsules
    if (m_config.show_player_capsules)
    {
        auto view = registry->view<PlayerComponent, TransformComponent>();
        for (auto entity : view)
        {
            auto& player    = view.get<PlayerComponent>(entity);
            auto& transform = view.get<TransformComponent>(entity);

            glm::vec3 pos = transform.position;
            glm::vec3 base = pos - glm::vec3(0.0f, player.capsule_half_height, 0.0f);
            glm::vec3 tip  = pos + glm::vec3(0.0f, player.capsule_half_height, 0.0f);

            dd.drawCapsule(base, tip, player.capsule_radius, m_config.capsule_color);
        }
    }

    // 3. Velocity vectors
    if (m_config.show_velocity)
    {
        auto view = registry->view<RigidBodyComponent, TransformComponent>();
        for (auto entity : view)
        {
            auto& rb        = view.get<RigidBodyComponent>(entity);
            auto& transform = view.get<TransformComponent>(entity);

            float speed = glm::length(rb.velocity);
            if (speed < 0.01f)
                continue;

            glm::vec3 start = transform.position;
            glm::vec3 end   = start + rb.velocity * m_config.velocity_scale;

            dd.drawLine(start, end, m_config.velocity_color);
            dd.drawPoint(end, 0.05f, m_config.velocity_color);
        }
    }

    // 4. RigidBody AABBs (for entities without a ColliderComponent — fallback to visual mesh bounds)
    if (m_config.show_rigidbody_aabb)
    {
        auto view = registry->view<RigidBodyComponent, MeshComponent, TransformComponent>();
        for (auto entity : view)
        {
            if (registry->all_of<ColliderComponent>(entity))
                continue;

            auto& mc        = view.get<MeshComponent>(entity);
            auto& transform = view.get<TransformComponent>(entity);

            if (!mc.m_mesh || !mc.m_mesh->is_valid)
                continue;

            glm::vec3 mesh_min, mesh_max;
            mc.m_mesh->getAABB(mesh_min, mesh_max);

            glm::vec3 world_min = mesh_min + transform.position;
            glm::vec3 world_max = mesh_max + transform.position;

            dd.drawAABB(world_min, world_max, m_config.rigidbody_color);
        }
    }
}
