#include "InspectorPanel.hpp"
#include "PanelUtils.hpp"
#include "ColliderWidgets.hpp"
#include "Components/Components.hpp"
#include "Graphics/LODSelector.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionTypes.hpp"
#include "Reflection/ReflectionWidgets.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <filesystem>

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
                          bool* out_unsaved, bool* out_edit_started,
                          bool* p_open)
{
    bool transform_dirty = false;

    // Helper lambdas
    auto markUnsaved = [&]() { if (out_unsaved) *out_unsaved = true; };
    auto markEditStarted = [&]() { if (out_edit_started) *out_edit_started = true; };

    ImGui::Begin("Inspector", p_open);
    PanelMaximizeButton();

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

            // Component context menu (right-click on header)
            if (ImGui::BeginPopupContextItem("##comp_ctx"))
            {
                if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Reset to Defaults"))
                {
                    resetComponentToDefaults(desc, comp);
                    markUnsaved();
                    if (desc.type_id == transform_type_id)
                        transform_dirty = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CLIPBOARD "  Copy Component"))
                {
                    copyComponentToClipboard(desc, comp);
                }
                bool can_paste = m_has_clipboard && m_clipboard_type_id == desc.type_id;
                if (ImGui::MenuItem(ICON_FA_PASTE "  Paste Values", nullptr, false, can_paste))
                {
                    pasteComponentFromClipboard(desc, comp);
                    markUnsaved();
                    if (desc.type_id == transform_type_id)
                        transform_dirty = true;
                }
                if (desc.removable)
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove"))
                        removed = true;
                }
                ImGui::EndPopup();
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
            const char* path = (it != mesh_path_cache.end()) ? it->second.c_str() : "(none)";

            float browse_w = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN " Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
            ImGui::LabelText("Path", "%s", path);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse"))
            {
                m_mesh_picker_entity = selected;
                m_mesh_picker_search[0] = '\0';
                refreshMeshFileCache();
                m_mesh_picker_open = true;
            }

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
                        mc.m_mesh->current_lod.load(std::memory_order_relaxed), mc.m_mesh->getLODCount() - 1);
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
        // Mesh context menu (right-click on header)
        if (ImGui::BeginPopupContextItem("##mesh_ctx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Remove"))
                removed = true;
            ImGui::EndPopup();
        }

        if (removed)
        {
            registry.remove<MeshComponent>(selected);
            transform_dirty = true;
            markUnsaved();
        }
        ImGui::Spacing();
    }

    // Mesh asset picker popup (rendered outside the mesh component section)
    drawMeshPickerPopup();

    // ---- Collider (custom UI — non-reflectable shared_ptr data) ----
    if (registry.all_of<ColliderComponent>(selected))
    {
        bool removed = false;
        if (drawComponentHeader("Collider", true, &removed))
        {
            auto& col = registry.get<ColliderComponent>(selected);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (drawColliderUI(col))
                markUnsaved();
            ImGui::PopItemWidth();
        }

        // Collider context menu (right-click on header)
        if (ImGui::BeginPopupContextItem("##collider_ctx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Remove"))
                removed = true;
            ImGui::EndPopup();
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

InspectorPanel::~InspectorPanel()
{
    clearClipboard();
}

void InspectorPanel::clearClipboard()
{
    // The buffer holds a placement-constructed instance whose destructor must run
    // before the bytes are freed or reused, otherwise std::string members leak.
    if (m_has_clipboard && m_clipboard_desc && m_clipboard_desc->destruct)
        m_clipboard_desc->destruct(m_clipboard_data.data());
    m_has_clipboard = false;
    m_clipboard_desc = nullptr;
    m_clipboard_type_id = 0;
    m_clipboard_data.clear();
}

void InspectorPanel::copyComponentToClipboard(const ComponentDescriptor& desc, void* src)
{
    clearClipboard();

    m_clipboard_type_id = desc.type_id;
    m_clipboard_desc = &desc;
    m_clipboard_data.resize(desc.size);

    // Construct a default in the buffer so any std::string members are valid
    desc.construct_default(m_clipboard_data.data());

    // Copy each property field-by-field
    for (const auto& prop : desc.properties)
    {
        void* src_field = static_cast<char*>(src) + prop.offset;
        void* dst_field = m_clipboard_data.data() + prop.offset;
        if (prop.type == EPropertyType::String)
            *static_cast<std::string*>(dst_field) = *static_cast<std::string*>(src_field);
        else
            std::memcpy(dst_field, src_field, prop.size);
    }
    m_has_clipboard = true;
}

void InspectorPanel::pasteComponentFromClipboard(const ComponentDescriptor& desc, void* dest)
{
    if (!m_has_clipboard || m_clipboard_type_id != desc.type_id) return;

    for (const auto& prop : desc.properties)
    {
        void* src_field = m_clipboard_data.data() + prop.offset;
        void* dst_field = static_cast<char*>(dest) + prop.offset;
        if (prop.type == EPropertyType::String)
            *static_cast<std::string*>(dst_field) = *static_cast<std::string*>(src_field);
        else
            std::memcpy(dst_field, src_field, prop.size);
    }
}

void InspectorPanel::resetComponentToDefaults(const ComponentDescriptor& desc, void* dest)
{
    assert(desc.size <= 4096);
    std::vector<uint8_t> temp(desc.size);
    desc.construct_default(temp.data());

    for (const auto& prop : desc.properties)
    {
        void* src_field = temp.data() + prop.offset;
        void* dst_field = static_cast<char*>(dest) + prop.offset;
        if (prop.type == EPropertyType::String)
            *static_cast<std::string*>(dst_field) = *static_cast<std::string*>(src_field);
        else
            std::memcpy(dst_field, src_field, prop.size);
    }

    desc.destruct(temp.data());
}

void InspectorPanel::refreshMeshFileCache()
{
    namespace fs = std::filesystem;
    m_mesh_file_cache.clear();

    if (!fs::exists(asset_base_path) || !fs::is_directory(asset_base_path))
        return;

    for (const auto& entry : fs::recursive_directory_iterator(asset_base_path))
    {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".obj" || ext == ".gltf" || ext == ".glb")
        {
            std::string p = entry.path().string();
            std::replace(p.begin(), p.end(), '\\', '/');
            m_mesh_file_cache.push_back(p);
        }
    }
    std::sort(m_mesh_file_cache.begin(), m_mesh_file_cache.end());
}

void InspectorPanel::drawMeshPickerPopup()
{
    if (m_mesh_picker_open)
    {
        ImGui::OpenPopup("##MeshPicker");
        m_mesh_picker_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("##MeshPicker", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::Text(ICON_FA_CUBE "  Select Mesh");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##mesh_search", "Search...", m_mesh_picker_search, sizeof(m_mesh_picker_search));
        ImGui::Spacing();

        std::string filter_lower;
        if (m_mesh_picker_search[0] != '\0')
        {
            filter_lower = m_mesh_picker_search;
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
        }

        float footer_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##mesh_list", ImVec2(0, -footer_h), true);

        for (const auto& mesh_path : m_mesh_file_cache)
        {
            if (!filter_lower.empty())
            {
                std::string path_lower = mesh_path;
                std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
                if (path_lower.find(filter_lower) == std::string::npos)
                    continue;
            }

            if (ImGui::Selectable((std::string(ICON_FA_CUBE "  ") + mesh_path).c_str()))
            {
                if (on_browse_mesh)
                    on_browse_mesh(m_mesh_picker_entity, mesh_path);
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndChild();

        if (ImGui::Button("Cancel", ImVec2(-1, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}
