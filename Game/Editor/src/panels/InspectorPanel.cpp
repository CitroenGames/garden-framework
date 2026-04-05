#include "InspectorPanel.hpp"
#include "Components/Components.hpp"
#include "Graphics/LODSelector.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>

bool InspectorPanel::drawComponentHeader(const char* label, bool can_remove, bool* removed, ImVec4 accent_color)
{
    (void)accent_color; // UE5 style: no accent bars

    ImGui::PushID(label);

    // Full-width darker header bar (UE5 style)
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.11f, 0.10f, 0.09f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.16f, 0.15f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.19f, 0.18f, 0.16f, 1.0f));

    bool open = ImGui::CollapsingHeader("##header", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    ImGui::PopStyleColor(3);

    bool header_hovered = ImGui::IsItemHovered();

    // Draw label with bold font
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImFont* bold = ImGuiManager::get().getBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::TextUnformatted(label);
    if (bold) ImGui::PopFont();

    // Remove button — only visible on hover
    if (can_remove && header_hovered)
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail - 5.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::SmallButton("X"))
            *removed = true;
        ImGui::PopStyleColor(3);
    }

    ImGui::PopID();
    return open;
}

bool InspectorPanel::draw(entt::registry& registry, entt::entity selected,
                          bool* out_unsaved, bool* out_edit_started)
{
    bool transform_dirty = false;

    // Helper lambdas
    auto markUnsaved = [&]() { if (out_unsaved) *out_unsaved = true; };
    auto markEditStarted = [&]() { if (out_edit_started) *out_edit_started = true; };

    ImGui::Begin("Inspector");

    if (selected == entt::null || !registry.valid(selected))
    {
        ImGui::TextDisabled("No entity selected.");
        ImGui::End();
        return false;
    }

    // Search/filter bar
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##inspector_filter", "Search components...", m_filter_buf, sizeof(m_filter_buf));
    ImGui::Spacing();

    // Tighter property spacing inside sections
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));

    // --- Tag (always present, not removable) ---
    if (auto* tag = registry.try_get<TagComponent>(selected))
    {
        bool unused = false;
        if (drawComponentHeader("Tag", false, &unused, ImVec4(0.86f, 0.86f, 0.86f, 1.0f)))
        {
            char buf[256];
            std::strncpy(buf, tag->name.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##tag_name", buf, sizeof(buf)))
            {
                tag->name = buf;
                markUnsaved();
            }
            if (ImGui::IsItemActivated()) markEditStarted();
        }
    }

    ImGui::Spacing();

    // --- Transform (always present, not removable) ---
    if (auto* t = registry.try_get<TransformComponent>(selected))
    {
        bool unused = false;
        if (drawComponentHeader("Transform", false, &unused, ImVec4(1.0f, 0.6f, 0.2f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::DragFloat3("Position", &t->position.x, 0.01f))
            { transform_dirty = true; markUnsaved(); }
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat3("Rotation", &t->rotation.x, 0.5f))
            { transform_dirty = true; markUnsaved(); }
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat3("Scale", &t->scale.x, 0.01f, 0.001f, 1000.0f))
            { transform_dirty = true; markUnsaved(); }
            if (ImGui::IsItemActivated()) markEditStarted();

            ImGui::PopItemWidth();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Mesh ---
    if (registry.all_of<MeshComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Mesh", true, &removed, ImVec4(0.3f, 0.9f, 0.9f, 1.0f)))
        {
            auto it = mesh_path_cache.find(selected);
            const char* path = (it != mesh_path_cache.end()) ? it->second.c_str() : "(unknown)";
            ImGui::LabelText("Path", "%s", path);

            auto& mc = registry.get<MeshComponent>(selected);
            if (mc.m_mesh)
            {
                if (ImGui::Checkbox("Visible", &mc.m_mesh->visible)) markUnsaved();
                ImGui::SameLine();
                if (ImGui::Checkbox("Transparent", &mc.m_mesh->transparent)) markUnsaved();
                if (ImGui::Checkbox("Casts Shadow", &mc.m_mesh->casts_shadow)) markUnsaved();
                ImGui::SameLine();
                if (ImGui::Checkbox("Culling", &mc.m_mesh->culling)) markUnsaved();

                // LOD info and controls (only show when mesh has LOD levels)
                if (mc.m_mesh->getLODCount() > 1)
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "LOD: %d/%d",
                        mc.m_mesh->current_lod, mc.m_mesh->getLODCount() - 1);
                    ImGui::SameLine();
                    ImGui::TextDisabled(mc.m_mesh->force_lod >= 0 ? "(forced)" : "(auto)");
                }
                if (mc.m_mesh->getLODCount() > 1)
                {
                    int max_lod = mc.m_mesh->getLODCount() - 1;
                    const char* preview = "Auto";
                    char lod_buf[16];
                    if (mc.m_mesh->force_lod >= 0)
                    {
                        snprintf(lod_buf, sizeof(lod_buf), "LOD %d", mc.m_mesh->force_lod);
                        preview = lod_buf;
                    }
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
                    if (ImGui::BeginCombo("Force LOD", preview))
                    {
                        if (ImGui::Selectable("Auto", mc.m_mesh->force_lod == -1))
                        { mc.m_mesh->force_lod = -1; markUnsaved(); }
                        for (int i = 0; i <= max_lod; ++i)
                        {
                            char label[16];
                            snprintf(label, sizeof(label), "LOD %d", i);
                            if (ImGui::Selectable(label, mc.m_mesh->force_lod == i))
                            { mc.m_mesh->force_lod = i; markUnsaved(); }
                        }
                        ImGui::EndCombo();
                    }

                    // LOD debug: show computed screen coverage and thresholds
                    auto* t = registry.try_get<TransformComponent>(selected);
                    if (t && mc.m_mesh->bounds_computed)
                    {
                        float coverage = LODSelector::computeScreenCoverage(
                            debug_cam_pos, t->position,
                            mc.m_mesh->aabb_min, mc.m_mesh->aabb_max,
                            debug_projection, t->scale);
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f),
                            "Coverage: %.3f", coverage);
                        for (int li = 0; li < static_cast<int>(mc.m_mesh->lod_levels.size()); ++li)
                        {
                            ImGui::TextDisabled("  LOD%d thr=%.3f gpu=%s",
                                li + 1, mc.m_mesh->lod_levels[li].screen_threshold,
                                mc.m_mesh->lod_levels[li].gpu_mesh ? "OK" : "NULL");
                        }
                    }
                    else if (t)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                            "bounds_computed=false");
                    }
                }
            }
        }
        if (removed)
        {
            registry.remove<MeshComponent>(selected);
            transform_dirty = true;
            markUnsaved();
        }
        ImGui::Spacing();
    }

    // --- Collider ---
    if (registry.all_of<ColliderComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Collider", true, &removed, ImVec4(0.3f, 0.9f, 0.3f, 1.0f)))
        {
            ImGui::TextDisabled("Collider component present");
        }
        if (removed)
        { registry.remove<ColliderComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- RigidBody ---
    if (auto* rb = registry.try_get<RigidBodyComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("RigidBody", true, &removed, ImVec4(1.0f, 0.5f, 0.2f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.001f, 100000.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::Checkbox("Apply Gravity", &rb->apply_gravity))
                markUnsaved();

            ImGui::PopItemWidth();
        }
        if (removed)
        { registry.remove<RigidBodyComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- Player ---
    if (auto* pc = registry.try_get<PlayerComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Player", true, &removed, ImVec4(0.3f, 0.5f, 1.0f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::DragFloat("Speed", &pc->speed, 0.1f, 0.0f, 100.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat("Jump Force", &pc->jump_force, 0.1f, 0.0f, 100.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat("Mouse Sensitivity", &pc->mouse_sensitivity, 0.01f, 0.01f, 10.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            ImGui::PopItemWidth();
        }
        if (removed)
        { registry.remove<PlayerComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- Freecam ---
    if (auto* fc = registry.try_get<FreecamComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Freecam", true, &removed, ImVec4(0.3f, 0.9f, 0.3f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::DragFloat("Move Speed", &fc->movement_speed, 0.1f, 0.0f, 100.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat("Fast Speed", &fc->fast_movement_speed, 0.5f, 0.0f, 500.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            if (ImGui::DragFloat("Mouse Sensitivity", &fc->mouse_sensitivity, 0.01f, 0.01f, 10.0f))
                markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();

            ImGui::PopItemWidth();
        }
        if (removed)
        { registry.remove<FreecamComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- Player Representation ---
    if (registry.all_of<PlayerRepresentationComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Player Representation", true, &removed, ImVec4(0.5f, 0.3f, 0.9f, 1.0f)))
        {
            ImGui::TextDisabled("Tracks another player entity");
        }
        if (removed)
        { registry.remove<PlayerRepresentationComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- Point Light ---
    if (auto* pl = registry.try_get<PointLightComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Point Light", true, &removed, ImVec4(1.0f, 0.85f, 0.2f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::ColorEdit3("Color", &pl->color.x)) markUnsaved();
            if (ImGui::DragFloat("Intensity", &pl->intensity, 0.1f, 0.0f, 100.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::DragFloat("Range", &pl->range, 0.1f, 0.1f, 1000.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::TreeNode("Attenuation"))
            {
                if (ImGui::DragFloat("Constant", &pl->constant_attenuation, 0.01f, 0.0f, 10.0f)) markUnsaved();
                if (ImGui::DragFloat("Linear", &pl->linear_attenuation, 0.001f, 0.0f, 2.0f)) markUnsaved();
                if (ImGui::DragFloat("Quadratic", &pl->quadratic_attenuation, 0.001f, 0.0f, 2.0f)) markUnsaved();
                ImGui::TreePop();
            }
            ImGui::PopItemWidth();
        }
        if (removed)
        { registry.remove<PointLightComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    // --- Spot Light ---
    if (auto* sl = registry.try_get<SpotLightComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Spot Light", true, &removed, ImVec4(1.0f, 0.8f, 0.15f, 1.0f)))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::ColorEdit3("Color", &sl->color.x)) markUnsaved();
            if (ImGui::DragFloat("Intensity", &sl->intensity, 0.1f, 0.0f, 100.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::DragFloat("Range", &sl->range, 0.1f, 0.1f, 1000.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::DragFloat("Inner Cone", &sl->inner_cone_angle, 0.5f, 0.0f, 90.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::DragFloat("Outer Cone", &sl->outer_cone_angle, 0.5f, 0.0f, 90.0f)) markUnsaved();
            if (ImGui::IsItemActivated()) markEditStarted();
            if (ImGui::TreeNode("Attenuation"))
            {
                if (ImGui::DragFloat("Constant", &sl->constant_attenuation, 0.01f, 0.0f, 10.0f)) markUnsaved();
                if (ImGui::DragFloat("Linear", &sl->linear_attenuation, 0.001f, 0.0f, 2.0f)) markUnsaved();
                if (ImGui::DragFloat("Quadratic", &sl->quadratic_attenuation, 0.001f, 0.0f, 2.0f)) markUnsaved();
                ImGui::TreePop();
            }
            ImGui::PopItemWidth();
        }
        if (removed)
        { registry.remove<SpotLightComponent>(selected); markUnsaved(); }
        ImGui::Spacing();
    }

    ImGui::PopStyleVar(); // ItemSpacing

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Add Component --- (blue-tinted UE5 style)
    float btn_width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.26f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.34f, 0.52f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.22f, 0.36f, 1.0f));
    if (ImGui::Button("+ Add Component", ImVec2(btn_width, 0)))
        ImGui::OpenPopup("AddComponentPopup");
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        if (!registry.all_of<MeshComponent>(selected))
            if (ImGui::MenuItem("Mesh"))
            { registry.emplace<MeshComponent>(selected); markUnsaved(); }

        if (!registry.all_of<ColliderComponent>(selected))
            if (ImGui::MenuItem("Collider"))
            { registry.emplace<ColliderComponent>(selected); markUnsaved(); }

        if (!registry.all_of<RigidBodyComponent>(selected))
            if (ImGui::MenuItem("RigidBody"))
            { registry.emplace<RigidBodyComponent>(selected); markUnsaved(); }

        if (!registry.all_of<PlayerComponent>(selected))
            if (ImGui::MenuItem("Player"))
            { registry.emplace<PlayerComponent>(selected); markUnsaved(); }

        if (!registry.all_of<FreecamComponent>(selected))
            if (ImGui::MenuItem("Freecam"))
            { registry.emplace<FreecamComponent>(selected); markUnsaved(); }

        ImGui::Separator();

        if (!registry.all_of<PointLightComponent>(selected))
            if (ImGui::MenuItem("Point Light"))
            { registry.emplace<PointLightComponent>(selected); markUnsaved(); }

        if (!registry.all_of<SpotLightComponent>(selected))
            if (ImGui::MenuItem("Spot Light"))
            { registry.emplace<SpotLightComponent>(selected); markUnsaved(); }

        ImGui::EndPopup();
    }

    ImGui::End();
    return transform_dirty;
}
