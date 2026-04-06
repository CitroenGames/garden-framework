#include "SceneHierarchyPanel.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "EditorIcons.hpp"
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

    // --- Search bar with icon ---
    ImGui::TextDisabled(ICON_FA_SEARCH);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Search entities...", m_filter_buf, sizeof(m_filter_buf));

    // --- Add Entity button ---
    if (ImGui::Button(ICON_FA_PLUS " Add Entity"))
        ImGui::OpenPopup("AddEntityPopup");

    if (ImGui::BeginPopup("AddEntityPopup"))
    {
        if (ImGui::MenuItem(ICON_FA_CIRCLE "  Empty Entity"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Entity");
            registry.emplace<TransformComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem(ICON_FA_CUBE "  Mesh Entity"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Mesh Entity");
            registry.emplace<TransformComponent>(e);
            registry.emplace<MeshComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem(ICON_FA_BOX "  Physical Entity"))
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
        if (ImGui::MenuItem(ICON_FA_USER "  Player"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Player");
            registry.emplace<TransformComponent>(e);
            registry.emplace<PlayerComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem(ICON_FA_CAMERA "  Freecam"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Freecam");
            registry.emplace<TransformComponent>(e);
            registry.emplace<FreecamComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Point Light");
            registry.emplace<TransformComponent>(e);
            registry.emplace<PointLightComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        if (ImGui::MenuItem(ICON_FA_BOLT "  Spot Light"))
        {
            auto e = registry.create();
            registry.emplace<TagComponent>(e, "New Spot Light");
            registry.emplace<TransformComponent>(e);
            registry.emplace<SpotLightComponent>(e);
            selected_entity = e;
            if (out_dirty) *out_dirty = true;
            if (out_unsaved) *out_unsaved = true;
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Entity count
    {
        size_t count = 0;
        auto count_view = registry.view<TagComponent>();
        for (auto e : count_view) { (void)e; count++; }
        ImGui::TextDisabled("(%zu entities)", count);
    }

    // --- Entity list ---
    // UE5-style selection highlight
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.35f, 0.55f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.40f, 0.60f, 0.80f));

    auto view = registry.view<TagComponent>();

    entt::entity to_delete = entt::null;
    entt::entity to_duplicate = entt::null;
    int visible_row = 0;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (auto entity : view)
    {
        const auto& tag = view.get<TagComponent>(entity);

        // Apply search filter
        if (!containsCaseInsensitive(tag.name, m_filter_buf))
            continue;

        bool is_selected = (entity == selected_entity);
        ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));

        // Alternating row backgrounds
        if (visible_row % 2 == 1)
        {
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            float row_height = ImGui::GetTextLineHeightWithSpacing();
            ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_height);
            draw_list->AddRectFilled(row_min, row_max, IM_COL32(255, 255, 255, 6));
        }
        visible_row++;

        // Entity type icon — Font Awesome icons with colored text
        const char* icon = ICON_FA_CIRCLE;
        ImVec4 icon_color(0.43f, 0.43f, 0.43f, 1.0f);

        if (registry.all_of<PrefabInstanceComponent>(entity))
        {
            icon = ICON_FA_PUZZLE_PIECE;
            icon_color = ImVec4(0.75f, 0.40f, 1.0f, 1.0f);
        }
        else if (registry.all_of<PlayerComponent>(entity))
        {
            icon = ICON_FA_USER;
            icon_color = ImVec4(0.39f, 0.55f, 0.90f, 1.0f);
        }
        else if (registry.all_of<FreecamComponent>(entity))
        {
            icon = ICON_FA_CAMERA;
            icon_color = ImVec4(0.39f, 0.78f, 0.39f, 1.0f);
        }
        else if (registry.all_of<PointLightComponent>(entity))
        {
            icon = ICON_FA_LIGHTBULB;
            icon_color = ImVec4(1.0f, 0.86f, 0.24f, 1.0f);
        }
        else if (registry.all_of<SpotLightComponent>(entity))
        {
            icon = ICON_FA_BOLT;
            icon_color = ImVec4(1.0f, 0.78f, 0.16f, 1.0f);
        }
        else if (registry.all_of<RigidBodyComponent>(entity))
        {
            icon = ICON_FA_BOX;
            icon_color = ImVec4(0.90f, 0.63f, 0.27f, 1.0f);
        }
        else if (registry.all_of<MeshComponent>(entity))
        {
            icon = ICON_FA_CUBE;
            icon_color = ImVec4(0.39f, 0.78f, 0.78f, 1.0f);
        }

        ImGui::TextColored(icon_color, "%s", icon);
        ImGui::SameLine();

        // Inline rename mode
        if (entity == m_renaming_entity)
        {
            if (!m_rename_focus_set)
            {
                ImGui::SetKeyboardFocusHere();
                m_rename_focus_set = true;
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
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
            // Reserve space for eye icon on the right
            float eye_width = 24.0f;
            float selectable_width = ImGui::GetContentRegionAvail().x - eye_width;

            if (ImGui::Selectable(tag.name.c_str(), is_selected, 0, ImVec2(selectable_width, 0)))
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

            // Visibility eye toggle (right-aligned)
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 18.0f);
            bool is_hidden = m_hidden_entities.count(entity) > 0;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_Text, is_hidden ? ImVec4(0.3f, 0.3f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            if (ImGui::SmallButton(is_hidden ? ICON_FA_EYE_SLASH "##vis" : ICON_FA_EYE "##vis"))
            {
                if (is_hidden)
                    m_hidden_entities.erase(entity);
                else
                    m_hidden_entities.insert(entity);
            }
            ImGui::PopStyleColor(4);
        }

        if (ImGui::BeginPopupContextItem("entity_ctx"))
        {
            if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate Entity"))
            {
                to_duplicate = entity;
            }
            if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE "  Save as Prefab"))
            {
                if (on_save_as_prefab)
                    on_save_as_prefab(entity);
            }
            if (ImGui::BeginMenu(ICON_FA_PLUS "  Add Component"))
            {
                static const uint32_t mesh_type_id     = entt::type_hash<MeshComponent>::value();
                static const uint32_t collider_type_id = entt::type_hash<ColliderComponent>::value();

                if (!registry.all_of<MeshComponent>(entity))
                    if (ImGui::MenuItem("Mesh"))
                    { registry.emplace<MeshComponent>(entity); if (out_unsaved) *out_unsaved = true; }

                if (!registry.all_of<ColliderComponent>(entity))
                    if (ImGui::MenuItem("Collider"))
                    { registry.emplace<ColliderComponent>(entity); if (out_unsaved) *out_unsaved = true; }

                if (reflection)
                {
                    bool need_separator = true;
                    for (const auto& desc : reflection->getAll())
                    {
                        if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                            continue;
                        if (desc.has(registry, entity))
                            continue;
                        if (need_separator) { ImGui::Separator(); need_separator = false; }
                        if (ImGui::MenuItem(desc.display_name))
                        {
                            desc.add(registry, entity);
                            if (out_unsaved) *out_unsaved = true;
                        }
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_PENCIL "  Rename"))
            {
                m_renaming_entity = entity;
                m_rename_focus_set = false;
                std::strncpy(m_rename_buf, tag.name.c_str(), sizeof(m_rename_buf) - 1);
                m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Entity"))
            {
                to_delete = entity;
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    ImGui::PopStyleColor(2); // Header, HeaderHovered

    // Click empty space in hierarchy to deselect
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
        selected_entity = entt::null;

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
        m_hidden_entities.erase(to_delete);
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

    if (auto* pl = registry.try_get<PointLightComponent>(source))
        registry.emplace<PointLightComponent>(new_entity, *pl);

    if (auto* sl = registry.try_get<SpotLightComponent>(source))
        registry.emplace<SpotLightComponent>(new_entity, *sl);

    selected_entity = new_entity;
    return new_entity;
}
