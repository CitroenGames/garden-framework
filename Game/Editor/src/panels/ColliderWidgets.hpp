#pragma once

#include "Components/Components.hpp"
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

// Shared collider editing UI used by InspectorPanel and PrefabEditorManager.
// Returns true if any value was changed.
inline bool drawColliderUI(ColliderComponent& col)
{
    bool changed = false;

    // --- Shape Type dropdown ---
    ImGui::Spacing();
    ImGui::TextDisabled("Shape");
    ImGui::Separator();

    int shape_int = static_cast<int>(col.shape_type);
    const char* preview = (shape_int >= 0 && shape_int < static_cast<int>(ColliderShapeType::COUNT))
        ? collider_shape_type_names[shape_int] : "Unknown";

    if (ImGui::BeginCombo("Shape Type", preview))
    {
        for (int i = 0; i < static_cast<int>(ColliderShapeType::COUNT); i++)
        {
            bool is_selected = (shape_int == i);
            if (ImGui::Selectable(collider_shape_type_names[i], is_selected))
            {
                col.shape_type = static_cast<ColliderShapeType>(i);
                changed = true;
            }
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Collision shape type");

    // --- Shape-specific parameters ---
    switch (col.shape_type)
    {
    case ColliderShapeType::Box:
        if (ImGui::DragFloat3("Half Extents", glm::value_ptr(col.box_half_extents), 0.01f, 0.001f, 1000.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Box half extents");
        break;

    case ColliderShapeType::Sphere:
        if (ImGui::DragFloat("Radius", &col.sphere_radius, 0.01f, 0.001f, 1000.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sphere radius");
        break;

    case ColliderShapeType::Capsule:
        if (ImGui::DragFloat("Half Height", &col.capsule_half_height, 0.01f, 0.001f, 100.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Capsule half height");
        if (ImGui::DragFloat("Radius", &col.capsule_radius, 0.01f, 0.001f, 100.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Capsule radius");
        break;

    case ColliderShapeType::Cylinder:
        if (ImGui::DragFloat("Half Height", &col.cylinder_half_height, 0.01f, 0.001f, 100.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cylinder half height");
        if (ImGui::DragFloat("Radius", &col.cylinder_radius, 0.01f, 0.001f, 100.0f))
            changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cylinder radius");
        break;

    case ColliderShapeType::Mesh:
    case ColliderShapeType::ConvexHull:
        ImGui::TextDisabled("Uses mesh from MeshComponent");
        break;

    default:
        break;
    }

    // --- Material properties (always shown) ---
    ImGui::Spacing();
    ImGui::TextDisabled("Material");
    ImGui::Separator();

    if (ImGui::DragFloat("Friction", &col.friction, 0.01f, 0.0f, 10.0f))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Surface friction");

    if (ImGui::DragFloat("Restitution", &col.restitution, 0.01f, 0.0f, 1.0f))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bounciness");

    return changed;
}
