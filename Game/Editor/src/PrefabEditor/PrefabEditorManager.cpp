#include "PrefabEditorManager.hpp"
#include "panels/ColliderWidgets.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Graphics/renderer.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionTypes.hpp"
#include "Reflection/ReflectionWidgets.hpp"
#include "Prefab/PrefabManager.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "EditorIcons.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <filesystem>
#include <algorithm>

// ── Initialization ──────────────────────────────────────────────────────────

void PrefabEditorManager::initialize(IRenderAPI* render_api, ReflectionRegistry* reflection)
{
    m_render_api = render_api;
    m_reflection = reflection;
}

// ── Open / Focus ────────────────────────────────────────────────────────────

void PrefabEditorManager::openPrefab(const std::string& prefab_path)
{
    std::string normalized = prefab_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // If already open and alive, focus it
    for (auto& inst : m_instances)
    {
        if (inst->prefab_path == normalized && inst->is_open)
        {
            ImGui::SetWindowFocus(inst->window_id.c_str());
            return;
        }
    }

    // Remove any stale (closed) instances for this path
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [&](const std::unique_ptr<PrefabEditorInstance>& inst) {
                if (inst->prefab_path == normalized && !inst->is_open)
                {
                    if (inst->viewport_id >= 0 && m_render_api)
                        m_render_api->destroyPIEViewport(inst->viewport_id);
                    return true;
                }
                return false;
            }),
        m_instances.end());

    // Create new instance
    auto inst = std::make_unique<PrefabEditorInstance>();
    inst->prefab_path = normalized;
    inst->id = m_next_id++;

    if (m_render_api)
        inst->viewport_id = m_render_api->createPIEViewport(inst->viewport_width, inst->viewport_height);

    loadPrefabIntoInstance(*inst);

    inst->window_id = "Prefab: " + inst->prefab_name + "##Prefab" + std::to_string(inst->id);

    m_instances.push_back(std::move(inst));
}

// ── Load prefab into isolated instance ──────────────────────────────────────

void PrefabEditorManager::loadPrefabIntoInstance(PrefabEditorInstance& inst)
{
    PrefabData data;
    if (!PrefabManager::loadPrefab(inst.prefab_path, data))
    {
        inst.prefab_name = "Error";
        return;
    }

    inst.prefab_name = data.name;

    if (data.json.contains("mesh") && data.json["mesh"].contains("path"))
        inst.mesh_path = data.json["mesh"]["path"].get<std::string>();
    if (data.json.contains("collider") && data.json["collider"].contains("mesh_path"))
        inst.collider_mesh_path = data.json["collider"]["mesh_path"].get<std::string>();

    inst.entity = PrefabManager::get().spawn(inst.registry, inst.prefab_path);

    if (inst.entity != entt::null)
    {
        auto* mc = inst.registry.try_get<MeshComponent>(inst.entity);
        if (mc && mc->m_mesh && mc->m_mesh->is_valid)
        {
            mc->m_mesh->computeBounds();
            inst.orbit.frameAABB(mc->m_mesh->aabb_min, mc->m_mesh->aabb_max, glm::radians(75.0f));
        }
    }
}

// ── Save prefab from instance ───────────────────────────────────────────────

void PrefabEditorManager::savePrefabFromInstance(PrefabEditorInstance& inst)
{
    if (inst.entity == entt::null || !inst.registry.valid(inst.entity))
        return;

    PrefabManager::get().savePrefab(
        inst.registry, inst.entity,
        inst.prefab_path,
        inst.mesh_path,
        inst.collider_mesh_path);

    inst.unsaved_changes = false;
    inst.window_id = "Prefab: " + inst.prefab_name + "##Prefab" + std::to_string(inst.id);
}

// ── Render 3D previews ──────────────────────────────────────────────────────

void PrefabEditorManager::renderAllPreviews()
{
    if (!m_render_api) return;

    for (auto& inst : m_instances)
    {
        if (!inst->is_open || inst->entity == entt::null)
            continue;

        auto* mc = inst->registry.try_get<MeshComponent>(inst->entity);
        if (!mc || !mc->m_mesh || !mc->m_mesh->gpu_mesh)
            continue;

        if (!m_render_api)
            continue;

        if (inst->viewport_width > 0 && inst->viewport_height > 0)
            m_render_api->setPIEViewportSize(inst->viewport_id, inst->viewport_width, inst->viewport_height);

        m_render_api->setActiveSceneTarget(inst->viewport_id);
        m_render_api->beginFrame();
        m_render_api->clear(glm::vec3(0.12f, 0.12f, 0.14f));

        camera preview_cam = inst->orbit.toCamera();
        m_render_api->setCamera(preview_cam);

        m_render_api->enableLighting(true);
        m_render_api->setLighting(
            glm::vec3(0.3f, 0.3f, 0.35f),
            glm::vec3(0.9f, 0.85f, 0.8f),
            glm::normalize(glm::vec3(-0.5f, -0.8f, -0.3f))
        );

        LightCBuffer empty_lights{};
        empty_lights.cameraPos = preview_cam.getPosition();
        m_render_api->setPointAndSpotLights(empty_lights);

        TransformComponent identity;
        renderer::render_mesh_with_api(*mc->m_mesh, identity, m_render_api);

        m_render_api->endSceneRender();
    }
}

// ── Draw all editor windows ─────────────────────────────────────────────────

void PrefabEditorManager::drawAll()
{
    for (auto& inst : m_instances)
    {
        if (inst->is_open)
            drawEditorWindow(*inst);
    }

    // Remove closed instances — no save_prompt gate, since the modal is inside the window
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [this](const std::unique_ptr<PrefabEditorInstance>& inst) {
                if (!inst->is_open)
                {
                    if (inst->viewport_id >= 0 && m_render_api)
                        m_render_api->destroyPIEViewport(inst->viewport_id);
                    return true;
                }
                return false;
            }),
        m_instances.end());
}

// ── Main editor window (single window, child regions) ───────────────────────

void PrefabEditorManager::drawEditorWindow(PrefabEditorInstance& inst)
{
    // Build title with dirty indicator
    std::string display_title = inst.unsaved_changes
        ? ("Prefab: " + inst.prefab_name + " *##Prefab" + std::to_string(inst.id))
        : inst.window_id;

    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar;

    bool window_open = inst.is_open;
    ImGui::Begin(display_title.c_str(), &window_open, flags);

    // Handle close
    if (!window_open)
    {
        if (inst.unsaved_changes)
        {
            inst.wants_close = true;
            inst.show_save_prompt = true;
        }
        else
        {
            inst.is_open = false;
            ImGui::End();
            return;
        }
    }

    // Save prompt modal (drawn inside this window, always visible)
    if (inst.show_save_prompt)
        drawSavePrompt(inst);

    // Toolbar in menu bar
    drawToolbar(inst);

    // --- Layout: [Components 20%] | [Viewport 50%] | [Details 30%] ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left_w = avail.x * 0.20f;
    float right_w = avail.x * 0.30f;
    float center_w = avail.x - left_w - right_w - 8.0f; // 8px for separators
    float panel_h = avail.y;

    // Left: Components
    ImGui::BeginChild("##prefab_components", ImVec2(left_w, panel_h), ImGuiChildFlags_Borders);
    drawComponentsPanel(inst);
    ImGui::EndChild();

    ImGui::SameLine();

    // Center: Viewport
    ImGui::BeginChild("##prefab_viewport", ImVec2(center_w, panel_h), ImGuiChildFlags_Borders);
    drawViewport(inst);
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Details
    ImGui::BeginChild("##prefab_details", ImVec2(right_w, panel_h), ImGuiChildFlags_Borders);
    drawDetailsPanel(inst);
    ImGui::EndChild();

    ImGui::End();
}

// ── Toolbar (in menu bar) ───────────────────────────────────────────────────

void PrefabEditorManager::drawToolbar(PrefabEditorInstance& inst)
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::MenuItem(ICON_FA_FILE " Save", "Ctrl+S"))
        savePrefabFromInstance(inst);

    ImGui::Separator();

    ImGui::BeginDisabled();
    ImGui::MenuItem(ICON_FA_GEAR " Compile");
    ImGui::EndDisabled();

    ImGui::Separator();

    ImGui::TextDisabled("%s", inst.prefab_name.c_str());
    ImGui::Separator();

    std::string filename = std::filesystem::path(inst.prefab_path).filename().string();
    ImGui::TextDisabled("(%s)", filename.c_str());

    ImGui::EndMenuBar();
}

// ── Components panel (drawn inside BeginChild) ──────────────────────────────

void PrefabEditorManager::drawComponentsPanel(PrefabEditorInstance& inst)
{
    if (inst.entity == entt::null || !inst.registry.valid(inst.entity))
    {
        ImGui::TextDisabled("Failed to load prefab");
        return;
    }

    ImFont* bold = ImGuiManager::get().getBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::Text(ICON_FA_PUZZLE_PIECE " Components");
    if (bold) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    static const uint32_t mesh_type_id = entt::type_hash<MeshComponent>::value();
    static const uint32_t collider_type_id = entt::type_hash<ColliderComponent>::value();
    static const uint32_t prefab_inst_type_id = entt::type_hash<PrefabInstanceComponent>::value();

    // Entity root node
    ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow
                                  | ImGuiTreeNodeFlags_SpanAvailWidth;

    bool root_open = ImGui::TreeNodeEx(("##root" + std::to_string(inst.id)).c_str(), root_flags,
        ICON_FA_PUZZLE_PIECE " %s", inst.prefab_name.c_str());

    if (root_open)
    {
        // Reflected components
        if (m_reflection)
        {
            for (const auto& desc : m_reflection->getAll())
            {
                if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                    continue;
                if (desc.type_id == prefab_inst_type_id)
                    continue;
                if (!desc.has(inst.registry, inst.entity))
                    continue;

                ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth
                                              | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                if (inst.has_selection && inst.selected_component_type == desc.type_id)
                    node_flags |= ImGuiTreeNodeFlags_Selected;

                ImGui::TreeNodeEx(desc.display_name, node_flags, ICON_FA_CUBE " %s", desc.display_name);

                if (ImGui::IsItemClicked())
                {
                    inst.selected_component_type = desc.type_id;
                    inst.has_selection = true;
                }
            }
        }

        // Mesh component
        if (inst.registry.all_of<MeshComponent>(inst.entity))
        {
            ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth
                                          | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (inst.has_selection && inst.selected_component_type == mesh_type_id)
                node_flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx("Mesh##comp", node_flags, ICON_FA_CUBE " Mesh");
            if (ImGui::IsItemClicked())
            {
                inst.selected_component_type = mesh_type_id;
                inst.has_selection = true;
            }
        }

        // Collider component
        if (inst.registry.all_of<ColliderComponent>(inst.entity))
        {
            ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth
                                          | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (inst.has_selection && inst.selected_component_type == collider_type_id)
                node_flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx("Collider##comp", node_flags, ICON_FA_BOX " Collider");
            if (ImGui::IsItemClicked())
            {
                inst.selected_component_type = collider_type_id;
                inst.has_selection = true;
            }
        }

        ImGui::TreePop();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Add Component button
    float btn_width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.26f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.34f, 0.52f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.22f, 0.36f, 1.0f));
    std::string popup_id = "AddCompPrefab##" + std::to_string(inst.id);
    if (ImGui::Button("+ Add Component", ImVec2(btn_width, 0)))
        ImGui::OpenPopup(popup_id.c_str());
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup(popup_id.c_str()))
    {
        if (!inst.registry.all_of<MeshComponent>(inst.entity))
        {
            if (ImGui::MenuItem("Mesh"))
            {
                inst.registry.emplace<MeshComponent>(inst.entity);
                inst.unsaved_changes = true;
            }
        }

        if (!inst.registry.all_of<ColliderComponent>(inst.entity))
        {
            if (ImGui::MenuItem("Collider"))
            {
                inst.registry.emplace<ColliderComponent>(inst.entity);
                inst.unsaved_changes = true;
            }
        }

        if (m_reflection)
        {
            bool need_separator = true;
            for (const auto& desc : m_reflection->getAll())
            {
                if (desc.type_id == mesh_type_id || desc.type_id == collider_type_id)
                    continue;
                if (desc.has(inst.registry, inst.entity))
                    continue;

                if (need_separator) { ImGui::Separator(); need_separator = false; }

                if (ImGui::MenuItem(desc.display_name))
                {
                    desc.add(inst.registry, inst.entity);
                    inst.unsaved_changes = true;
                }
            }
        }

        ImGui::EndPopup();
    }
}

// ── Details panel (drawn inside BeginChild) ─────────────────────────────────

void PrefabEditorManager::drawDetailsPanel(PrefabEditorInstance& inst)
{
    ImFont* bold = ImGuiManager::get().getBoldFont();

    if (bold) ImGui::PushFont(bold);
    ImGui::Text(ICON_FA_GEAR " Details");
    if (bold) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    if (inst.entity == entt::null || !inst.registry.valid(inst.entity))
    {
        ImGui::TextDisabled("No prefab loaded");
        return;
    }

    if (!inst.has_selection)
    {
        ImGui::TextDisabled("Select a component");
        return;
    }

    static const uint32_t mesh_type_id = entt::type_hash<MeshComponent>::value();
    static const uint32_t collider_type_id = entt::type_hash<ColliderComponent>::value();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));

    // Mesh component (custom UI)
    if (inst.selected_component_type == mesh_type_id)
    {
        if (bold) ImGui::PushFont(bold);
        ImGui::Text(ICON_FA_CUBE " Mesh");
        if (bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        if (inst.registry.all_of<MeshComponent>(inst.entity))
        {
            const char* path = inst.mesh_path.empty() ? "(no mesh)" : inst.mesh_path.c_str();
            ImGui::LabelText("Path", "%s", path);

            auto& mc = inst.registry.get<MeshComponent>(inst.entity);
            if (mc.m_mesh)
            {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);

                if (ImGui::Checkbox("Visible", &mc.m_mesh->visible)) inst.unsaved_changes = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Transparent", &mc.m_mesh->transparent)) inst.unsaved_changes = true;
                if (ImGui::Checkbox("Casts Shadow", &mc.m_mesh->casts_shadow)) inst.unsaved_changes = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Culling", &mc.m_mesh->culling)) inst.unsaved_changes = true;

                if (mc.m_mesh->getLODCount() > 1)
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "LOD Levels: %d",
                        mc.m_mesh->getLODCount());

                    int max_lod = mc.m_mesh->getLODCount() - 1;
                    const char* preview = "Auto";
                    char lod_buf[16];
                    if (mc.m_mesh->force_lod >= 0)
                    {
                        snprintf(lod_buf, sizeof(lod_buf), "LOD %d", mc.m_mesh->force_lod);
                        preview = lod_buf;
                    }
                    if (ImGui::BeginCombo("Force LOD", preview))
                    {
                        if (ImGui::Selectable("Auto", mc.m_mesh->force_lod == -1))
                        { mc.m_mesh->force_lod = -1; inst.unsaved_changes = true; }
                        for (int i = 0; i <= max_lod; ++i)
                        {
                            char label[16];
                            snprintf(label, sizeof(label), "LOD %d", i);
                            if (ImGui::Selectable(label, mc.m_mesh->force_lod == i))
                            { mc.m_mesh->force_lod = i; inst.unsaved_changes = true; }
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("Vertices: %zu", mc.m_mesh->vertices_len);
                if (mc.m_mesh->bounds_computed)
                {
                    glm::vec3 size = mc.m_mesh->aabb_max - mc.m_mesh->aabb_min;
                    ImGui::TextDisabled("Size: %.2f x %.2f x %.2f", size.x, size.y, size.z);
                }

                ImGui::PopItemWidth();
            }
            else
            {
                ImGui::TextDisabled("No mesh loaded");
            }
        }
        else
        {
            ImGui::TextDisabled("Mesh component removed");
            inst.has_selection = false;
        }
    }
    // Collider component (custom UI)
    else if (inst.selected_component_type == collider_type_id)
    {
        if (bold) ImGui::PushFont(bold);
        ImGui::Text(ICON_FA_BOX " Collider");
        if (bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        if (inst.registry.all_of<ColliderComponent>(inst.entity))
        {
            auto& col = inst.registry.get<ColliderComponent>(inst.entity);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (drawColliderUI(col))
                inst.unsaved_changes = true;
            ImGui::PopItemWidth();
        }
        else
        {
            ImGui::TextDisabled("Collider component removed");
            inst.has_selection = false;
        }
    }
    // Reflected component (data-driven UI)
    else if (m_reflection)
    {
        const auto* desc = m_reflection->findByTypeId(inst.selected_component_type);
        if (desc && desc->has(inst.registry, inst.entity))
        {
            void* comp = desc->get(inst.registry, inst.entity);
            if (comp)
            {
                if (bold) ImGui::PushFont(bold);
                ImGui::Text("%s", desc->display_name);
                if (bold) ImGui::PopFont();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);

                bool edit_started = false;
                if (drawReflectedComponent(*desc, comp, &edit_started))
                    inst.unsaved_changes = true;

                ImGui::PopItemWidth();
            }
        }
        else
        {
            ImGui::TextDisabled("Component no longer present");
            inst.has_selection = false;
        }
    }

    ImGui::PopStyleVar();
}

// ── Viewport (drawn inside BeginChild) ──────────────────────────────────────

void PrefabEditorManager::drawViewport(PrefabEditorInstance& inst)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int new_w = static_cast<int>(avail.x);
    int new_h = static_cast<int>(avail.y);
    if (new_w > 0 && new_h > 0)
    {
        inst.viewport_width = new_w;
        inst.viewport_height = new_h;
    }

    if (inst.viewport_id >= 0 && m_render_api)
    {
        ImTextureID tex = (ImTextureID)m_render_api->getPIEViewportTextureID(inst.viewport_id);
        if (tex)
        {
            ImGui::Image(tex, avail);

            if (ImGui::IsItemHovered())
            {
                // Left-drag: orbit
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    inst.orbit.orbit(delta.x, delta.y);
                }

                // Middle-drag: pan
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                {
                    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);

                    float pan_speed = inst.orbit.distance * 0.002f;
                    float cos_yaw = cosf(inst.orbit.yaw);
                    float sin_yaw = sinf(inst.orbit.yaw);

                    glm::vec3 right(-cos_yaw, 0.0f, sin_yaw);
                    glm::vec3 up(0.0f, 1.0f, 0.0f);

                    inst.orbit.target -= right * delta.x * pan_speed;
                    inst.orbit.target += up * delta.y * pan_speed;
                }

                // Scroll: zoom
                float scroll = ImGui::GetIO().MouseWheel;
                if (scroll != 0.0f)
                    inst.orbit.zoom(scroll);
            }
        }
        else
        {
            ImGui::TextDisabled("Rendering preview...");
        }
    }
    else
    {
        ImGui::TextDisabled("No mesh to preview");
    }

    // Frame button at bottom-left
    ImVec2 child_size = ImGui::GetWindowSize();
    if (child_size.y > 40.0f)
    {
        ImGui::SetCursorPos(ImVec2(8.0f, child_size.y - 30.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.9f));
        if (ImGui::SmallButton(ICON_FA_CROSSHAIRS " Frame"))
        {
            auto* mc = inst.registry.try_get<MeshComponent>(inst.entity);
            if (mc && mc->m_mesh && mc->m_mesh->bounds_computed)
                inst.orbit.frameAABB(mc->m_mesh->aabb_min, mc->m_mesh->aabb_max, glm::radians(75.0f));
        }
        ImGui::PopStyleColor(2);
    }
}

// ── Save prompt dialog ──────────────────────────────────────────────────────

void PrefabEditorManager::drawSavePrompt(PrefabEditorInstance& inst)
{
    std::string modal_id = "Save Changes?##Prefab" + std::to_string(inst.id);

    if (!ImGui::IsPopupOpen(modal_id.c_str()))
        ImGui::OpenPopup(modal_id.c_str());

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(modal_id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Save changes to \"%s\" before closing?", inst.prefab_name.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Save", ImVec2(80, 0)))
        {
            savePrefabFromInstance(inst);
            inst.show_save_prompt = false;
            inst.wants_close = false;
            inst.is_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Don't Save", ImVec2(100, 0)))
        {
            inst.show_save_prompt = false;
            inst.wants_close = false;
            inst.is_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(80, 0)))
        {
            inst.show_save_prompt = false;
            inst.wants_close = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ── Shutdown ────────────────────────────────────────────────────────────────

void PrefabEditorManager::shutdown()
{
    if (m_render_api)
    {
        for (auto& inst : m_instances)
        {
            if (inst->viewport_id >= 0)
                m_render_api->destroyPIEViewport(inst->viewport_id);
        }
    }
    m_instances.clear();
}
