#include "ReflectionWidgets.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <string>
#include <cstring>

static EPropertyWidget resolveWidget(const PropertyDescriptor& prop)
{
    if (prop.meta.widget != EPropertyWidget::Auto)
        return prop.meta.widget;

    // If specifier is VisibleAnywhere, force read-only
    if (prop.meta.specifier == EPropertySpecifier::VisibleAnywhere)
        return EPropertyWidget::ReadOnly;

    // Infer from type
    switch (prop.type)
    {
    case EPropertyType::Float:  return EPropertyWidget::DragFloat;
    case EPropertyType::Int:    return EPropertyWidget::DragInt;
    case EPropertyType::Bool:   return EPropertyWidget::Checkbox;
    case EPropertyType::String: return EPropertyWidget::InputText;
    case EPropertyType::Vec2:   return EPropertyWidget::DragFloat2;
    case EPropertyType::Vec3:   return EPropertyWidget::DragFloat3;
    case EPropertyType::Vec4:   return EPropertyWidget::DragFloat4;
    case EPropertyType::Enum:   return EPropertyWidget::Enum;
    default:                    return EPropertyWidget::ReadOnly;
    }
}

bool drawReflectedProperty(const PropertyDescriptor& prop, void* component)
{
    void* field_ptr = static_cast<char*>(component) + prop.offset;
    const char* label = prop.meta.display_name ? prop.meta.display_name : prop.name;
    EPropertyWidget widget = resolveWidget(prop);

    // Read-only wrapper
    bool read_only = (widget == EPropertyWidget::ReadOnly);
    if (read_only)
    {
        ImGui::BeginDisabled();
        // Resolve the actual widget to draw
        widget = EPropertyWidget::Auto;
        switch (prop.type)
        {
        case EPropertyType::Float:  widget = EPropertyWidget::DragFloat;  break;
        case EPropertyType::Int:    widget = EPropertyWidget::DragInt;    break;
        case EPropertyType::Bool:   widget = EPropertyWidget::Checkbox;   break;
        case EPropertyType::String: widget = EPropertyWidget::InputText;  break;
        case EPropertyType::Vec2:   widget = EPropertyWidget::DragFloat2; break;
        case EPropertyType::Vec3:   widget = EPropertyWidget::DragFloat3; break;
        case EPropertyType::Vec4:   widget = EPropertyWidget::DragFloat4; break;
        default: break;
        }
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
        if (prop.meta.enum_names && prop.meta.enum_count > 0)
        {
            const char* preview = (*val >= 0 && *val < prop.meta.enum_count)
                ? prop.meta.enum_names[*val] : "Unknown";
            if (ImGui::BeginCombo(label, preview))
            {
                for (int i = 0; i < prop.meta.enum_count; i++)
                {
                    bool selected = (*val == i);
                    if (ImGui::Selectable(prop.meta.enum_names[i], selected))
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

    if (read_only)
        ImGui::EndDisabled();

    // Tooltip
    if (prop.meta.tooltip && prop.meta.tooltip[0] != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", prop.meta.tooltip);

    return changed;
}

bool drawReflectedComponent(const ComponentDescriptor& desc, void* component)
{
    bool any_changed = false;
    const char* current_category = nullptr;

    for (uint32_t i = 0; i < desc.property_count; i++)
    {
        const PropertyDescriptor& prop = desc.properties[i];

        // Category separator
        if (prop.meta.category && prop.meta.category[0] != '\0')
        {
            if (!current_category || std::strcmp(current_category, prop.meta.category) != 0)
            {
                current_category = prop.meta.category;
                ImGui::Spacing();
                ImGui::TextDisabled("%s", current_category);
                ImGui::Separator();
            }
        }

        if (drawReflectedProperty(prop, component))
            any_changed = true;
    }

    return any_changed;
}
