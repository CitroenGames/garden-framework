#include "SceneHierarchyPanel.hpp"
#include "Components/Components.hpp"
#include "imgui.h"
#include <algorithm>
#include <string>
#include <cstring>

static bool containsCaseInsensitive(const std::string& str, const char* filter)
{
    if (!filter || filter[0] == '\0') return true;
    std::string lower_str = str;
    std::string lower_filter(filter);
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
    return lower_str.find(lower_filter) != std::string::npos;
}

void SceneHierarchyPanel::draw(entt::registry& registry, bool* out_dirty, bool* out_unsaved)
{
    ImGui::Begin("Scene Hierarchy");

    // --- Search bar ---
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Search entities...", m_filter_buf, sizeof(m_filter_buf));

    // --- Add Entity button ---
    if (ImGui::Button("+ Add Entity"))
        ImGui::OpenPopup("AddEntityPopup");

    if (ImGui::BeginPopup("AddEntityPopup"))
    {
        if (ImGui::MenuItem("Empty Entity"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Entity");
            registry.emplace<TransformComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem("Mesh Entity"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Mesh Entity");
            registry.emplace<TransformComponent>(e);
            registry.emplace<MeshComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem("Physical Entity"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Physical Entity");
            registry.emplace<TransformComponent>(e);
            registry.emplace<RigidBodyComponent>(e);
            registry.emplace<ColliderComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem("Player"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Player");
            registry.emplace<TransformComponent>(e);
            registry.emplace<PlayerComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem("Freecam"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Freecam");
            registry.emplace<TransformComponent>(e);
            registry.emplace<FreecamComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // --- Entity list ---
    auto view = registry.view<TagComponent>();

    entt::entity to_delete = entt::null;
    entt::entity to_duplicate = entt::null;

    for (auto entity : view)
    {
        const auto& tag = view.get<TagComponent>(entity);

        // Apply search filter
        if (!containsCaseInsensitive(tag.name, m_filter_buf))
            continue;

        bool is_selected = (entity == selected_entity);
        ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));

        // Entity type icon — draw colored shape via ImDrawList
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 icon_pos = ImGui::GetCursorScreenPos();
        float line_h = ImGui::GetTextLineHeight();
        float radius = 5.0f;
        ImVec2 center(icon_pos.x + radius + 1.0f, icon_pos.y + line_h * 0.5f);

        if (registry.all_of<PlayerComponent>(entity))
        {
            // Filled circle — blue
            draw_list->AddCircleFilled(center, radius, IM_COL32(77, 128, 255, 255));
        }
        else if (registry.all_of<FreecamComponent>(entity))
        {
            // Filled triangle — green
            draw_list->AddTriangleFilled(
                ImVec2(center.x, center.y - radius),
                ImVec2(center.x - radius, center.y + radius * 0.7f),
                ImVec2(center.x + radius, center.y + radius * 0.7f),
                IM_COL32(77, 230, 77, 255));
        }
        else if (registry.all_of<RigidBodyComponent>(entity))
        {
            // Filled square — orange
            draw_list->AddRectFilled(
                ImVec2(center.x - radius + 1, center.y - radius + 1),
                ImVec2(center.x + radius - 1, center.y + radius - 1),
                IM_COL32(255, 153, 51, 255));
        }
        else if (registry.all_of<MeshComponent>(entity))
        {
            // Filled diamond — cyan
            draw_list->AddQuadFilled(
                ImVec2(center.x, center.y - radius),
                ImVec2(center.x + radius, center.y),
                ImVec2(center.x, center.y + radius),
                ImVec2(center.x - radius, center.y),
                IM_COL32(77, 230, 230, 255));
        }
        else
        {
            // Unfilled circle — grey
            draw_list->AddCircle(center, radius, IM_COL32(128, 128, 128, 255), 0, 1.5f);
        }

        ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, line_h));
        ImGui::SameLine();

        // Inline rename mode
        if (entity == m_renaming_entity)
        {
            if (!m_rename_focus_set)
            {
                ImGui::SetKeyboardFocusHere();
                m_rename_focus_set = true;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##rename", m_rename_buf, sizeof(m_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                // Enter pressed — apply rename
                auto* rename_tag = registry.try_get<TagComponent>(m_renaming_entity);
                if (rename_tag)
                {
                    rename_tag->name = m_rename_buf;
                    if (out_unsaved) *out_unsaved = true;
                }
                m_renaming_entity = entt::null;
            }
            // Click away — cancel rename
            if (!ImGui::IsItemActive() && m_rename_focus_set && m_renaming_entity != entt::null)
            {
                m_renaming_entity = entt::null;
            }
        }
        else
        {
            if (ImGui::Selectable(tag.name.c_str(), is_selected))
            {
                selected_entity = entity;
            }

            // Double-click to start inline rename
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                m_renaming_entity = entity;
                m_rename_focus_set = false;
                std::strncpy(m_rename_buf, tag.name.c_str(), sizeof(m_rename_buf) - 1);
                m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
            }
        }

        if (ImGui::BeginPopupContextItem("entity_ctx"))
        {
            if (ImGui::MenuItem("Duplicate Entity"))
            {
                to_duplicate = entity;
            }
            if (ImGui::MenuItem("Rename"))
            {
                m_renaming_entity = entity;
                m_rename_focus_set = false;
                std::strncpy(m_rename_buf, tag.name.c_str(), sizeof(m_rename_buf) - 1);
                m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Entity"))
            {
                to_delete = entity;
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // Deferred duplication
    if (registry.valid(to_duplicate))
    {
        duplicateEntity(registry, to_duplicate);
        if (out_dirty) *out_dirty = true;
        if (out_unsaved) *out_unsaved = true;
    }

    // Deferred deletion
    if (registry.valid(to_delete))
    {
        if (selected_entity == to_delete)
            selected_entity = entt::null;
        registry.destroy(to_delete);
        if (out_dirty) *out_dirty = true;
        if (out_unsaved) *out_unsaved = true;
    }

    ImGui::End();
}

entt::entity SceneHierarchyPanel::duplicateEntity(entt::registry& registry, entt::entity source)
{
    if (!registry.valid(source))
        return entt::null;

    auto new_entity = registry.create();

    if (auto* tag = registry.try_get<TagComponent>(source))
        registry.emplace<TagComponent>(new_entity, tag->name + " (Copy)");

    if (auto* t = registry.try_get<TransformComponent>(source))
        registry.emplace<TransformComponent>(new_entity, *t);

    if (auto* mc = registry.try_get<MeshComponent>(source))
        registry.emplace<MeshComponent>(new_entity, *mc);

    if (auto* rb = registry.try_get<RigidBodyComponent>(source))
        registry.emplace<RigidBodyComponent>(new_entity, *rb);

    if (auto* col = registry.try_get<ColliderComponent>(source))
        registry.emplace<ColliderComponent>(new_entity, *col);

    if (auto* pc = registry.try_get<PlayerComponent>(source))
        registry.emplace<PlayerComponent>(new_entity, *pc);

    if (auto* fc = registry.try_get<FreecamComponent>(source))
        registry.emplace<FreecamComponent>(new_entity, *fc);

    if (auto* pr = registry.try_get<PlayerRepresentationComponent>(source))
        registry.emplace<PlayerRepresentationComponent>(new_entity, *pr);

    selected_entity = new_entity;
    return new_entity;
}
