#include "InspectorPanel.hpp"
#include "Components/Components.hpp"
#include "Graphics/LODSelector.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionWidgets.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>

bool InspectorPanel::drawComponentHeader(const char* label, bool can_remove, bool* removed, ImVec4 accent_color)
{
    (void)accent_color; // UE5 style: no accent bars

    ImGui::PushID(label);

    // Full-width darker header bar (UE5 style)
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));

    bool open = ImGui::CollapsingHeader("##header", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    ImGui::PopStyleColor(3);

    // Draw label with bold font
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImFont* bold = ImGuiManager::get().getBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::TextUnformatted(label);
    if (bold) ImGui::PopFont();

    // Remove button
    if (can_remove)
    {
        float btn_pos = ImGui::GetWindowContentRegionMax().x - 20.0f;
        ImGui::SameLine(btn_pos);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
        if (ImGui::SmallButton(ICON_FA_XMARK))
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

    // Search/filter bar with icon
    ImGui::TextDisabled(ICON_FA_SEARCH);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##inspector_filter", "Search components...", m_filter_buf, sizeof(m_filter_buf));
    ImGui::Spacing();

    // Tighter property spacing inside sections
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));

    // Type IDs for components with custom UI (not reflection-driven)
    static const uint32_t mesh_type_id     = entt::type_hash<MeshComponent>::value();
    static const uint32_t collider_type_id = entt::type_hash<ColliderComponent>::value();
    static const uint32_t transform_type_id = entt::type_hash<TransformComponent>::value();

    // ---- Reflection-driven components ----
    if (reflection)
    {
        for (const auto& desc : reflection->getAll())
        {
            // Skip non-reflected custom-UI components
            if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                continue;

            if (!desc.has(registry, selected))
                continue;

            void* comp = desc.get(registry, selected);
            if (!comp) continue;

            // Filter by search text
            if (m_filter_buf[0] != '\0')
            {
                // Case-insensitive check against display name
                bool match = false;
                const char* haystack = desc.display_name;
                const char* needle = m_filter_buf;
                // Simple case-insensitive substring
                for (const char* h = haystack; *h; ++h)
                {
                    const char* hi = h;
                    const char* ni = needle;
                    while (*hi && *ni && ((*hi | 32) == (*ni | 32))) { ++hi; ++ni; }
                    if (!*ni) { match = true; break; }
                }
                if (!match) continue;
            }

            bool removed = false;
            if (drawComponentHeader(desc.display_name, desc.removable, &removed))
            {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);

                bool edit_started = false;
                if (drawReflectedComponent(desc, comp, &edit_started))
                {
                    markUnsaved();
                    if (desc.type_id == transform_type_id)
                        transform_dirty = true;
                }
                if (edit_started)
                    markEditStarted();

                ImGui::PopItemWidth();
            }

            if (removed && desc.removable)
            {
                desc.remove(registry, selected);
                markUnsaved();
            }

            ImGui::Spacing();
        }
    }

    // ---- Mesh (custom UI — non-reflectable shared_ptr data) ----
    if (registry.all_of<MeshComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Mesh", true, &removed))
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

    // ---- Collider (custom UI — non-reflectable shared_ptr data) ----
    if (registry.all_of<ColliderComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Collider", true, &removed))
        {
            ImGui::TextDisabled("Collider component present");
        }
        if (removed)
        { registry.remove<ColliderComponent>(selected); markUnsaved(); }
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
        // Custom non-reflected components
        if (!registry.all_of<MeshComponent>(selected))
            if (ImGui::MenuItem("Mesh"))
            { registry.emplace<MeshComponent>(selected); markUnsaved(); }

        if (!registry.all_of<ColliderComponent>(selected))
            if (ImGui::MenuItem("Collider"))
            { registry.emplace<ColliderComponent>(selected); markUnsaved(); }

        // Reflected components from registry
        if (reflection)
        {
            bool need_separator = true;
            for (const auto& desc : reflection->getAll())
            {
                if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                    continue;

                if (desc.has(registry, selected))
                    continue;

                if (need_separator) { ImGui::Separator(); need_separator = false; }

                if (ImGui::MenuItem(desc.display_name))
                {
                    desc.add(registry, selected);
                    markUnsaved();
                }
            }
        }

        ImGui::EndPopup();
    }

    ImGui::End();
    return transform_dirty;
}
