#include "InspectorPanel.hpp"
#include "Components/Components.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>

bool InspectorPanel::drawComponentHeader(const char* label, bool can_remove, bool* removed, ImVec4 accent_color)
{
    ImGui::PushID(label);

    // Slightly tinted header background
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));

    bool open = ImGui::CollapsingHeader("##header", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    ImGui::PopStyleColor(3);

    // Draw accent bar on the left edge of the header
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 h_min = ImGui::GetItemRectMin();
    ImVec2 h_max = ImGui::GetItemRectMax();
    ImU32 accent_col = IM_COL32((int)(accent_color.x * 255), (int)(accent_color.y * 255),
                                (int)(accent_color.z * 255), 255);
    draw_list->AddRectFilled(h_min, ImVec2(h_min.x + 3.0f, h_max.y), accent_col);

    // Draw colored dot indicator
    float dot_y = (h_min.y + h_max.y) * 0.5f;
    draw_list->AddCircleFilled(ImVec2(h_min.x + 14.0f, dot_y), 4.0f, accent_col);

    // Draw label after the dot
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
    ImGui::TextUnformatted(label);

    // Remove button on the right — rounded pill style
    if (can_remove)
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail - 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::SmallButton("X"))
            *removed = true;
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Add Component ---
    float btn_width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add Component", ImVec2(btn_width, 0)))
        ImGui::OpenPopup("AddComponentPopup");

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

        ImGui::EndPopup();
    }

    ImGui::End();
    return transform_dirty;
}
