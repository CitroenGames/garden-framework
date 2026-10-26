#include "ReflectionWidgets.hpp"
#include "ReflectionPropertyOps.hpp"
#include "Utils/FileDialog.hpp"
#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <glm/glm.hpp>
#include <string>
#include <cstring>
#include <utility>

namespace
{
    const char* assetPathDialogFilter(const PropertyDescriptor& prop)
    {
        if (prop.name.find("heightmap") != std::string::npos ||
            prop.name.find("albedo") != std::string::npos ||
            prop.name.find("texture") != std::string::npos)
        {
            return "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.dds)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.dds\0All Files (*.*)\0*.*\0";
        }

        return "Asset Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.dds;*.obj;*.gltf;*.glb)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.dds;*.obj;*.gltf;*.glb\0All Files (*.*)\0*.*\0";
    }

    std::string normalizeDialogAssetPath(const std::string& path)
    {
        namespace fs = std::filesystem;

        if (path.empty())
            return path;

        fs::path selected(path);
        std::error_code ec;
        if (!selected.is_absolute())
            selected = fs::absolute(selected, ec);

        if (!ec)
        {
            std::error_code canonical_ec;
            fs::path canonical_selected = fs::weakly_canonical(selected, canonical_ec);
            if (!canonical_ec)
                selected = canonical_selected;

            std::error_code cwd_ec;
            fs::path cwd = fs::current_path(cwd_ec);
            if (!cwd_ec)
                cwd = fs::weakly_canonical(cwd, cwd_ec);
            if (!cwd_ec)
            {
                std::error_code relative_ec;
                fs::path relative = fs::relative(selected, cwd, relative_ec);
                if (!relative_ec && !relative.empty())
                {
                    auto it = relative.begin();
                    if (it == relative.end() || it->string() != "..")
                    {
                        std::string out = relative.string();
                        std::replace(out.begin(), out.end(), '\\', '/');
                        return out;
                    }
                }
            }
        }

        std::string out = selected.string();
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }
}

bool drawReflectedProperty(const PropertyDescriptor& prop, void* component,
                           bool* out_edit_started)
{
    void* field_ptr = ReflectionPropertyOps::propertyData(prop, component);
    if (!field_ptr)
        return false;

    const char* label = prop.meta.display_name.empty() ? prop.name.c_str() : prop.meta.display_name.c_str();
    if (prop.type == EPropertyType::Array) { ImGui::TextDisabled("%s: [%zu]", label, ReflectionPropertyOps::arraySize(prop, component)); return false; }
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
    case EPropertyWidget::AssetPath:
    {
        auto* val = static_cast<std::string*>(field_ptr);
        char buf[512];
        std::strncpy(buf, val->c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        ImGui::PushID(label);

        const char* button_label = "Open...";
        const ImGuiStyle& style = ImGui::GetStyle();
        const float button_w = ImGui::CalcTextSize(button_label).x + style.FramePadding.x * 2.0f;
        const float input_w = std::max(64.0f, ImGui::CalcItemWidth() - button_w - style.ItemSpacing.x);

        ImGui::SetNextItemWidth(input_w);
        if (ImGui::InputText("##asset_path", buf, sizeof(buf)))
        {
            *val = buf;
            changed = true;
        }
        bool asset_path_edit_started = ImGui::IsItemActivated();
        const bool input_hovered = ImGui::IsItemHovered();

        ImGui::SameLine();
        bool button_clicked = ImGui::Button(button_label);
        const bool button_hovered = ImGui::IsItemHovered();
        if (button_clicked)
        {
            const std::string selected_path =
                FileDialog::openFile("Select Asset", assetPathDialogFilter(prop));
            if (!selected_path.empty())
            {
                std::string normalized_path = normalizeDialogAssetPath(selected_path);
                if (*val != normalized_path)
                {
                    *val = std::move(normalized_path);
                    changed = true;
                    asset_path_edit_started = true;
                }
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(label);

        if (out_edit_started && asset_path_edit_started)
            *out_edit_started = true;

        if (!prop.meta.tooltip.empty() &&
            (input_hovered || button_hovered || ImGui::IsItemHovered()))
        {
            ImGui::SetTooltip("%s", prop.meta.tooltip.c_str());
        }

        ImGui::PopID();
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
