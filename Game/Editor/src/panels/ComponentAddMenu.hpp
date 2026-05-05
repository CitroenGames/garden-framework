#pragma once

#include "Components/Components.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionTypes.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <cstdint>
#include <string>

namespace EditorComponentAddMenu
{
    inline bool isBuiltInEngineComponent(const ComponentDescriptor& desc)
    {
        return desc.source_id.empty() || desc.source_id == "engine";
    }

    template<typename OnAdded>
    bool draw(entt::registry& registry, entt::entity entity, ReflectionRegistry* reflection, OnAdded&& on_added)
    {
        static const uint32_t mesh_type_id = entt::type_hash<MeshComponent>::value();
        static const uint32_t collider_type_id = entt::type_hash<ColliderComponent>::value();

        bool changed = false;
        size_t built_in_available = 0;

        ImGui::SeparatorText("Built-in Engine");

        if (!registry.all_of<MeshComponent>(entity))
        {
            ++built_in_available;
            if (ImGui::MenuItem("Mesh"))
            {
                registry.emplace<MeshComponent>(entity);
                on_added();
                changed = true;
            }
        }

        if (!registry.all_of<ColliderComponent>(entity))
        {
            ++built_in_available;
            if (ImGui::MenuItem("Collider"))
            {
                registry.emplace<ColliderComponent>(entity);
                on_added();
                changed = true;
            }
        }

        if (reflection)
        {
            for (const auto& desc : reflection->getAll())
            {
                if (!isBuiltInEngineComponent(desc))
                    continue;
                if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                    continue;

                if ((desc.has && desc.has(registry, entity)) || !desc.add)
                    continue;

                ++built_in_available;
                if (ImGui::MenuItem(desc.display_name.c_str()))
                {
                    desc.add(registry, entity);
                    on_added();
                    changed = true;
                }
            }
        }

        if (built_in_available == 0)
            ImGui::MenuItem("All built-in components already added", nullptr, false, false);

        ImGui::SeparatorText("Custom");

        size_t custom_total = 0;
        size_t custom_available = 0;

        if (reflection)
        {
            for (const auto& desc : reflection->getAll())
            {
                if (isBuiltInEngineComponent(desc))
                    continue;

                ++custom_total;
                if ((desc.has && desc.has(registry, entity)) || !desc.add)
                    continue;

                ++custom_available;
                std::string label = desc.display_name;
                if (!desc.source_id.empty())
                    label += "  [" + desc.source_id + "]";

                if (ImGui::MenuItem(label.c_str()))
                {
                    desc.add(registry, entity);
                    on_added();
                    changed = true;
                }

                if (ImGui::IsItemHovered() && !desc.source_id.empty())
                    ImGui::SetTooltip("Source: %s", desc.source_id.c_str());
            }
        }

        if (custom_available == 0)
        {
            const char* label = custom_total == 0
                ? "No custom components registered"
                : "All custom components already added";
            ImGui::MenuItem(label, nullptr, false, false);
        }

        return changed;
    }
}
