#include "ReflectionWidgets.hpp"
#include "ReflectionPropertyOps.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <string>
#include <cstring>

bool drawReflectedProperty(const PropertyDescriptor& prop, void* component,
                           bool* out_edit_started)
{
    void* field_ptr = ReflectionPropertyOps::propertyData(prop, component);
    if (!field_ptr)
        return false;

    const char* label = prop.meta.display_name.empty() ? prop.name.c_str() : prop.meta.display_name.c_str();
    EPropertyWidget widget = ReflectionPropertyOps::resolveWidget(prop);

    // Read-only wrapper
    bool read_only = (widget == EPropertyWidget::ReadOnly);
    if (read_only)
    {
        ImGui::BeginDisabled();
        widget = ReflectionPropertyOps::defaultWidgetForType(prop.type);
    }

    bool changed = false;
    float speed = prop.meta.drag_speed;
    float v_min = prop.meta.has_clamp ? prop.meta.clamp_min : 0.0f;
    float v_max = prop.meta.has_clamp ? prop.meta.clamp_max : 0.0f;

    switch (widget)
    {
    case EPropertyWidget::DragFloat:
    {
        auto* val = static_cast<float*>(field_ptr);
        if (prop.meta.has_clamp)
            changed = ImGui::DragFloat(label, val, speed, v_min, v_max);
        else
            changed = ImGui::DragFloat(label, val, speed);
        break;
    }
    case EPropertyWidget::SliderFloat:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::SliderFloat(label, val, v_min, v_max);
        break;
    }
    case EPropertyWidget::DragInt:
    {
        auto* val = static_cast<int*>(field_ptr);
        int imin = static_cast<int>(v_min);
        int imax = static_cast<int>(v_max);
        if (prop.meta.has_clamp)
            changed = ImGui::DragInt(label, val, speed, imin, imax);
        else
            changed = ImGui::DragInt(label, val, speed);
        break;
    }
    case EPropertyWidget::SliderInt:
    {
        auto* val = static_cast<int*>(field_ptr);
        changed = ImGui::SliderInt(label, val, static_cast<int>(v_min), static_cast<int>(v_max));
        break;
    }
    case EPropertyWidget::Checkbox:
    {
        auto* val = static_cast<bool*>(field_ptr);
        changed = ImGui::Checkbox(label, val);
        break;
    }
    case EPropertyWidget::InputText:
    {
        auto* val = static_cast<std::string*>(field_ptr);
        char buf[512];
        std::strncpy(buf, val->c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(label, buf, sizeof(buf)))
        {
            *val = buf;
            changed = true;
        }
        break;
    }
    case EPropertyWidget::DragFloat2:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::DragFloat2(label, val, speed);
        break;
    }
    case EPropertyWidget::DragFloat3:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::DragFloat3(label, val, speed);
        break;
    }
    case EPropertyWidget::DragFloat4:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::DragFloat4(label, val, speed);
        break;
    }
    case EPropertyWidget::ColorEdit3:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::ColorEdit3(label, val);
        break;
    }
    case EPropertyWidget::ColorEdit4:
    {
        auto* val = static_cast<float*>(field_ptr);
        changed = ImGui::ColorEdit4(label, val);
        break;
    }
    case EPropertyWidget::Enum:
    {
        auto* val = static_cast<int*>(field_ptr);
        if (!prop.meta.enum_names.empty())
        {
            const char* preview = (*val >= 0 && *val < static_cast<int>(prop.meta.enum_names.size()))
                ? prop.meta.enum_names[static_cast<size_t>(*val)].c_str() : "Unknown";
            if (ImGui::BeginCombo(label, preview))
            {
                for (int i = 0; i < static_cast<int>(prop.meta.enum_names.size()); i++)
                {
                    bool selected = (*val == i);
                    if (ImGui::Selectable(prop.meta.enum_names[static_cast<size_t>(i)].c_str(), selected))
                    {
                        *val = i;
                        changed = true;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        break;
    }
    default:
        ImGui::TextDisabled("%s (unsupported widget)", label);
        break;
    }

    // Track edit-started for undo snapshots
    if (out_edit_started && ImGui::IsItemActivated())
        *out_edit_started = true;

    if (read_only)
        ImGui::EndDisabled();

    // Tooltip
    if (!prop.meta.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", prop.meta.tooltip.c_str());

    return changed;
}

bool drawReflectedComponent(const ComponentDescriptor& desc, void* component,
                            bool* out_edit_started)
{
    bool any_changed = false;
    std::string current_category;

    for (const auto& prop : desc.properties)
    {
        // Category separator
        if (!prop.meta.category.empty())
        {
            if (current_category != prop.meta.category)
            {
                current_category = prop.meta.category;
                ImGui::Spacing();
                ImGui::TextDisabled("%s", current_category.c_str());
                ImGui::Separator();
            }
        }

        if (drawReflectedProperty(prop, component, out_edit_started))
            any_changed = true;
    }

    return any_changed;
}
