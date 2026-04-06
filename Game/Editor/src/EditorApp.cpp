#include "EditorApp.hpp"
#include "EditorIcons.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "UI/RmlUiManager.h"
#include "Console/Console.hpp"
#include "Console/ConVar.hpp"
#include "Debug/DebugDraw.hpp"
#include "Utils/Log.hpp"
#include "Utils/FileDialog.hpp"
#include "Utils/EnginePaths.hpp"
#include "Components/Components.hpp"
#include "Components/PrefabInstanceComponent.hpp"
#include "Prefab/PrefabManager.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Assets/LODMeshSerializer.hpp"
#include "Project/ProjectManager.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include <SDL.h>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
static constexpr const char* PIE_GAME_EXE_NAME   = "Game.exe";
static constexpr const char* PIE_SERVER_EXE_NAME  = "Server.exe";
#else
static constexpr const char* PIE_GAME_EXE_NAME   = "Game";
static constexpr const char* PIE_SERVER_EXE_NAME  = "Server";
#endif

bool EditorApp::initialize(RenderAPIType api_type)
{
    std::strncpy(m_open_path_buf, "assets/levels/", sizeof(m_open_path_buf) - 1);
    std::strncpy(m_save_path_buf, "assets/levels/", sizeof(m_save_path_buf) - 1);

    EE::CLog::Init();
    Console::get().initialize();
    InitializeDefaultCVars();
    ConVarRegistry::get().loadConfig("config.cfg");

    int win_w = CVAR_INT(window_width);
    int win_h = CVAR_INT(window_height);
    if (win_w <= 0) win_w = 1600;
    if (win_h <= 0) win_h = 900;

    m_app = Application(win_w, win_h, 60, 75.0f, api_type);
    if (!m_app.initialize("Garden Level Editor", false))
    {
        LOG_ENGINE_FATAL("Failed to initialize Application");
        return false;
    }

    // Restore maximized state from config
    if (CVAR_BOOL(window_maximized))
        SDL_MaximizeWindow(m_app.getWindow());

    IRenderAPI* render_api = m_app.getRenderAPI();
    if (!render_api)
    {
        LOG_ENGINE_FATAL("Failed to get render API");
        return false;
    }

    if (!ImGuiManager::get().initialize(m_app.getWindow(), render_api, api_type))
    {
        LOG_ENGINE_FATAL("Failed to initialize ImGui");
        return false;
    }

    // Initialize RmlUi
    if (!RmlUiManager::get().initialize(m_app.getWindow(), render_api, api_type))
    {
        LOG_ENGINE_WARN("Failed to initialize RmlUi - continuing without UI");
    }

    // Disable built-in console overlay since we have our own panel
    ImGuiManager::get().setShowConsole(false);

    m_world.initializePhysics();

    m_renderer = renderer(render_api);

    // Apply graphics CVars from config.cfg
    render_api->setFXAAEnabled(CVAR_BOOL(r_fxaa));
    render_api->setShadowQuality(CVAR_INT(r_shadowquality));
    render_api->enableLighting(CVAR_BOOL(r_lighting));
    m_renderer.setDepthPrepassEnabled(CVAR_BOOL(r_depthprepass));
    m_renderer.setBVHEnabled(CVAR_BOOL(r_frustumculling));

    // Set up content browser callback
    m_content_browser.on_open_level = [this](const std::string& path) { openLevel(path); };

    // Wire up toolbar callbacks for PIE
    m_toolbar.callbacks.on_play   = [this]() { beginPlay(); };
    m_toolbar.callbacks.on_pause  = [this]() { pausePlay(); };
    m_toolbar.callbacks.on_resume = [this]() { resumePlay(); };
    m_toolbar.callbacks.on_stop   = [this]() { stopPlay(); };
    m_toolbar.callbacks.on_eject  = [this]() { ejectFromPlay(); };
    m_toolbar.callbacks.on_return = [this]() { returnToPlay(); };

    // Check if project has a game module DLL for network PIE
    {
        std::string dll_path = m_project_manager.getAbsoluteModulePath();
        m_toolbar.has_game_module = !dll_path.empty() && std::filesystem::exists(dll_path);
    }

    m_viewport.toolbar = &m_toolbar;
    m_viewport.show_toolbar = &m_show_toolbar;

    // Content browser drag-drop: spawn mesh entity when dropped onto viewport
    m_viewport.on_mesh_dropped = [this](const std::string& mesh_path) {
        IRenderAPI* api = m_app.getRenderAPI();
        if (!api) return;

        // Take undo snapshot before creating the entity
        m_undo.snapshotIfNeeded([this]() { return buildLevelDataFromECS(); });

        // Derive entity name from filename stem
        std::filesystem::path p(mesh_path);
        std::string entity_name = p.stem().string();

        // Create entity with Tag + Transform + MeshComponent
        auto entity = m_world.registry.create();
        m_world.registry.emplace<TagComponent>(entity, entity_name);
        m_world.registry.emplace<TransformComponent>(entity);
        auto& mc = m_world.registry.emplace<MeshComponent>(entity);

        // Load mesh with materials
        auto mesh_ptr = std::make_shared<mesh>(mesh_path, api);
        if (mesh_ptr->is_valid)
        {
            mesh_ptr->uploadToGPU(api);
            mc.m_mesh = mesh_ptr;
        }

        // Update mesh path cache so save/serialize works
        m_inspector.mesh_path_cache[entity] = mesh_path;

        // Select the new entity
        m_hierarchy.selected_entity = entity;

        m_state.unsaved_changes = true;
        m_renderer.markBVHDirty();
    };

    // Register engine reflection and wire up inspector
    registerEngineReflection(m_reflection);
    m_inspector.reflection = &m_reflection;

    // Initialize prefab manager (singleton, same pattern as SceneManager)
    PrefabManager::get().initialize(&m_reflection, render_api);

    // Prefab drag-drop: spawn prefab entity when dropped onto viewport
    m_viewport.on_prefab_dropped = [this](const std::string& prefab_path) {
        m_undo.snapshotIfNeeded([this]() { return buildLevelDataFromECS(); });

        auto entity = PrefabManager::get().spawn(m_world.registry, prefab_path);
        if (entity != entt::null)
        {
            // Update mesh path cache so save/serialize works
            PrefabData data;
            if (PrefabManager::loadPrefab(prefab_path, data))
            {
                if (data.json.contains("mesh") && data.json["mesh"].contains("path"))
                    m_inspector.mesh_path_cache[entity] = data.json["mesh"]["path"].get<std::string>();
            }

            m_hierarchy.selected_entity = entity;
            m_state.unsaved_changes = true;
            m_renderer.markBVHDirty();
        }
    };

    // Content browser: double-click prefab to spawn
    m_content_browser.on_spawn_prefab = [this](const std::string& prefab_path) {
        m_undo.snapshotIfNeeded([this]() { return buildLevelDataFromECS(); });

        auto entity = PrefabManager::get().spawn(m_world.registry, prefab_path);
        if (entity != entt::null)
        {
            PrefabData data;
            if (PrefabManager::loadPrefab(prefab_path, data))
            {
                if (data.json.contains("mesh") && data.json["mesh"].contains("path"))
                    m_inspector.mesh_path_cache[entity] = data.json["mesh"]["path"].get<std::string>();
            }

            m_hierarchy.selected_entity = entity;
            m_state.unsaved_changes = true;
            m_renderer.markBVHDirty();
        }
    };

    // Hierarchy: save entity as prefab file
    m_hierarchy.on_save_as_prefab = [this](entt::entity entity) {
        std::string path = FileDialog::saveFile("Save as Prefab",
            "Prefab Files (*.prefab)\0*.prefab\0All Files (*.*)\0*.*\0");
        if (path.empty()) return;

        // Ensure .prefab extension
        if (path.size() < 7 || path.substr(path.size() - 7) != ".prefab")
            path += ".prefab";

        // Get mesh path from cache
        std::string mesh_path;
        auto it = m_inspector.mesh_path_cache.find(entity);
        if (it != m_inspector.mesh_path_cache.end())
            mesh_path = it->second;

        // Get collider mesh path from original level entity if available
        std::string collider_path;
        if (auto* tag = m_world.registry.try_get<TagComponent>(entity))
        {
            const LevelEntity* orig = findOriginalLevelEntity(tag->name);
            if (orig)
                collider_path = orig->collider_mesh_path;
        }

        PrefabManager::get().savePrefab(
            m_world.registry, entity,
            path, mesh_path, collider_path);
    };

    m_navmesh_panel.registry = &m_world.registry;
    m_physics_debug_panel.registry = &m_world.registry;

    // Asset scanner: scan and process new/changed mesh assets
    m_content_browser.asset_scanner = &m_asset_scanner;
    m_lod_settings_panel.asset_scanner = &m_asset_scanner;

    // Content browser: double-click on mesh opens LOD settings
    m_content_browser.on_open_mesh = [this](const std::string& path) {
        m_lod_settings_panel.open(std::filesystem::path(path));
    };

    // Content browser: single-click on mesh triggers preview
    m_model_preview.render_api = render_api;
    m_content_browser.on_preview_mesh = [this](const std::string& path) {
        m_model_preview.setPreviewMesh(path);
    };

    // Hot-reload LODs into live meshes after generation
    m_lod_settings_panel.on_lods_generated = [this](const std::string& mesh_path) {
        reloadLODsForMesh(mesh_path);
    };

    // If --project was passed, load it directly.
    // Otherwise, show the project browser and wait for the user to pick one.
    if (!m_project_path.empty())
    {
        if (m_project_manager.loadProject(m_project_path))
        {
            std::filesystem::current_path(m_project_manager.getProjectRoot());
            LOG_ENGINE_INFO("Project '{}' loaded from '{}'",
                           m_project_manager.getDescriptor().name,
                           m_project_manager.getProjectRoot());

            if (!m_project_manager.getDescriptor().default_level.empty())
                openLevel(m_project_manager.getDescriptor().default_level);
        }
        else
        {
            LOG_ENGINE_ERROR("Failed to load project: {}", m_project_path);
        }
    }
    else
    {
        // Run the project browser — blocks until a project is selected or user quits
        if (!runProjectBrowser())
        {
            m_running = false;
            return true; // clean exit, not an error
        }
    }

    m_asset_scanner.scanDirectory("assets");
    m_asset_scanner.processAllPending();

    SDL_SetRelativeMouseMode(SDL_FALSE);

    m_running = true;

    LOG_ENGINE_INFO("Editor initialized successfully");
    return true;
}

void EditorApp::run()
{
    Uint32 last_ticks = SDL_GetTicks();

    while (m_running)
    {
        // Drain Metal autorelease pool each frame to prevent ObjC temporary object leaks.
        // On non-Metal backends this is a no-op passthrough.
        m_app.getRenderAPI()->executeWithAutoreleasePool([&]() {
        Uint32 now = SDL_GetTicks();
        m_delta_time = (now - last_ticks) / 1000.0f;
        last_ticks = now;

        // Reset per-frame mouse accumulators
        m_mouse_dx = 0.0f;
        m_mouse_dy = 0.0f;

        m_undo.beginFrame();

        processEvents();

        // --- Simulation tick (when active and running) ---
        if (m_state.isSimulationRunning())
        {
            if (m_network_pie_active)
            {
                // Network PIE: tick server (if listen server), then all clients
                if (m_state.network_pie.net_mode == PIENetMode::ListenServer)
                    m_game_module.serverUpdate(m_delta_time);

                // Tick Player 1 (main editor viewport)
                m_game_module.update(m_delta_time);
                m_renderer.markBVHDirty();

                // Tick additional in-editor PIE clients
                for (auto& inst : m_pie_clients)
                {
                    if (inst && inst->initialized)
                        inst->game_module.update(m_delta_time);
                }
            }
            else if (m_game_sim)
            {
                // Standalone: existing GameSimulation path
                if (m_mouse_captured_for_game && m_game_input_manager)
                {
                    float mx = m_game_input_manager->get_mouse_delta_x();
                    float my = m_game_input_manager->get_mouse_delta_y();
                    if (mx != 0.0f || my != 0.0f)
                        m_game_sim->handleMouseMotion(my, mx);
                }

                m_game_sim->update(m_delta_time);
                m_renderer.markBVHDirty();
            }
        }

        // --- Editor camera update (editing or ejected) ---
        if (!m_state.isSimulationActive() || m_state.play_mode == PlayMode::Ejected)
        {
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            m_editor_cam.update(m_delta_time, m_right_mouse, m_mouse_dx, m_mouse_dy, keys);
        }

        applyLightingFromMetadata();

        // Update debug draw (tick persistent lines)
        DebugDraw::get().update(m_delta_time);

        // Draw grid if enabled (only in editing mode)
        if (m_state.show_grid && !m_state.isSimulationActive())
            renderGrid();

        // NavMesh debug visualization (submit lines before scene render)
        m_navmesh_panel.drawDebugVisualization();

        // Physics debug visualization
        m_physics_debug_panel.drawDebugVisualization();

        // --- Choose which camera to render with ---
        camera& render_camera = chooseRenderCamera();

        // --- Phase 1: Render 3D scene to viewport texture ---
        IRenderAPI* render_api = m_app.getRenderAPI();
        render_api->setViewportSize(m_viewport.width, m_viewport.height);
        m_renderer.render_scene_to_texture(m_world.registry, render_camera);

        // --- Phase 1b: Render additional PIE client viewports ---
        for (auto& inst : m_pie_clients)
        {
            if (!inst || !inst->initialized || inst->viewport_id < 0)
                continue;

            // Resize PIE viewport if needed
            render_api->setPIEViewportSize(inst->viewport_id, inst->viewport_width, inst->viewport_height);

            // Redirect the next render to this PIE viewport's render target
            render_api->setActiveSceneTarget(inst->viewport_id);
            render_api->setViewportSize(inst->viewport_width, inst->viewport_height);

            // Render the client's world using its camera
            m_renderer.render_scene_to_texture(inst->client_world.registry,
                                                inst->client_world.world_camera);

            // setActiveSceneTarget(-1) is called inside endSceneRender() automatically
        }

        // Restore main viewport size
        if (!m_pie_clients.empty())
            render_api->setViewportSize(m_viewport.width, m_viewport.height);

        // Update editor state stats (after scene render so stats are current)
        m_state.fps = ImGui::GetIO().Framerate;
        m_state.delta_time = m_delta_time;
        m_state.total_entities = m_renderer.getTotalEntities();
        m_state.visible_entities = m_renderer.getVisibleEntities();
        m_state.draw_calls = m_renderer.getDrawCalls();
        m_state.current_save_path = m_current_save_path;

        // Update window title with dirty indicator
        {
            std::string title = "Garden Level Editor";
            if (!m_current_save_path.empty())
                title += " - " + m_current_save_path;
            if (m_state.unsaved_changes)
                title += " *";
            if (title != m_last_window_title)
            {
                SDL_SetWindowTitle(m_app.getWindow(), title.c_str());
                m_last_window_title = title;
            }
        }

        // --- Phase 2: Build ImGui UI ---
        ImGuiManager::get().newFrame();
        ImGuizmo::BeginFrame();
        RmlUiManager::get().beginFrame();

        renderDockspace();

        if (m_show_ui)
        {
            bool bvh_dirty = false;

            // Viewport panel with embedded toolbar, gizmo, and click-to-select
            if (m_show_viewport)
            {
                ImTextureID tex = (ImTextureID)render_api->getViewportTextureID();
                GizmoResult gizmo = m_viewport.draw(tex, m_state,
                    m_world.registry, m_hierarchy.selected_entity,
                    chooseRenderCamera(), render_api, m_renderer.getSceneBVH());
                if (gizmo.drag_started)
                    m_undo.snapshotIfNeeded([this]() { return buildLevelDataFromECS(); });
                if (gizmo.transform_changed)
                {
                    bvh_dirty = true;
                    m_state.unsaved_changes = true;
                }
            }

            // --- PIE client viewports (additional players, rendered as dockable windows) ---
            for (auto& inst : m_pie_clients)
            {
                if (!inst || !inst->initialized || inst->viewport_id < 0)
                    continue;

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                bool open = true;
                ImGui::Begin(inst->window_title.c_str(), &open,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                // Track available size for this viewport
                ImVec2 avail = ImGui::GetContentRegionAvail();
                int new_w = static_cast<int>(avail.x);
                int new_h = static_cast<int>(avail.y);
                if (new_w > 0 && new_h > 0)
                {
                    inst->viewport_width = new_w;
                    inst->viewport_height = new_h;
                }

                // Display the PIE viewport texture
                ImTextureID pie_tex = (ImTextureID)render_api->getPIEViewportTextureID(inst->viewport_id);
                if (pie_tex)
                    ImGui::Image(pie_tex, avail);

                // Show a colored border to indicate this is a PIE client
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 win_min = ImGui::GetWindowPos();
                ImVec2 win_max = ImVec2(win_min.x + ImGui::GetWindowSize().x,
                                        win_min.y + ImGui::GetWindowSize().y);
                dl->AddRect(win_min, win_max, IM_COL32(51, 204, 51, 180), 0.0f, 0, 2.0f);

                ImGui::End();
                ImGui::PopStyleVar();
            }

            if (m_show_hierarchy)
                m_hierarchy.draw(m_world.registry, &bvh_dirty, &m_state.unsaved_changes);

            if (m_show_inspector)
            {
                // Feed camera info for LOD debug display
                m_inspector.debug_cam_pos = chooseRenderCamera().getPosition();
                m_inspector.debug_projection = render_api->getProjectionMatrix();

                bool edit_started = false;
                bool transform_changed = m_inspector.draw(m_world.registry, m_hierarchy.selected_entity,
                                                          &m_state.unsaved_changes, &edit_started);
                if (transform_changed)
                    bvh_dirty = true;
                if (edit_started)
                    m_undo.snapshotIfNeeded([this]() { return buildLevelDataFromECS(); });
            }

            if (m_show_level_settings)
                m_level_settings.draw();

            if (m_show_console)
                m_console.draw();

            if (m_show_content_browser)
                m_content_browser.draw();

            if (m_show_model_preview)
                m_model_preview.draw();

            if (m_show_navmesh_panel)
                m_navmesh_panel.draw();

            if (m_show_physics_debug)
                m_physics_debug_panel.draw();

            // LOD settings panel (opens when double-clicking a mesh in content browser)
            m_lod_settings_panel.draw();

            // Viewport overlay (not docked, transparent)
            m_viewport_overlay.draw(m_state);

            // Status bar (fixed at bottom)
            if (m_show_status_bar)
            {
                m_status_bar.network_pie_active = m_network_pie_active;
                m_status_bar.spawned_processes = m_pie_processes.countRunning();
                m_status_bar.draw(m_state);
            }

            renderOpenDialog();
            renderSaveAsDialog();
            renderPackageDialog();
            renderEditorSettings();

            if (bvh_dirty)
                m_renderer.markBVHDirty();
        }

        // --- Phase 3: Render ImGui to screen ---
        ImGui::Render();
        render_api->renderUI();
        m_app.swapBuffers();

        Uint32 frame_end = SDL_GetTicks();
        m_app.lockFramerate(now, frame_end);
        }); // executeWithAutoreleasePool
    }
}

void EditorApp::reloadLODsForMesh(const std::string& mesh_path)
{
    IRenderAPI* render_api = m_app.getRenderAPI();
    if (!render_api) return;

    std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(mesh_path);
    Assets::AssetMetadata metadata;
    if (!Assets::AssetMetadataSerializer::load(metadata, meta_path) || !metadata.lod_enabled)
        return;

    std::string mesh_dir = std::filesystem::path(mesh_path).parent_path().string();
    if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
        mesh_dir += "/";

    for (const auto& [entity, cached_path] : m_inspector.mesh_path_cache)
    {
        if (cached_path != mesh_path) continue;
        if (!m_world.registry.valid(entity)) continue;

        auto* mc = m_world.registry.try_get<MeshComponent>(entity);
        if (!mc || !mc->m_mesh) continue;

        mesh& m = *mc->m_mesh;

        // Clear existing LOD levels (LODLevel destructor frees GPU resources)
        m.lod_levels.clear();
        m.current_lod = 0;

        // Load LOD levels from .lodbin files
        for (size_t i = 1; i < metadata.lod_levels.size(); ++i)
        {
            const auto& lod_info = metadata.lod_levels[i];
            if (lod_info.file_path.empty()) continue;

            std::string lod_path = mesh_dir + lod_info.file_path;
            Assets::LODMeshData lod_data;
            if (Assets::LODMeshSerializer::load(lod_data, lod_path))
            {
                mesh::LODLevel level;
                level.screen_threshold = lod_info.screen_threshold;
                level.vertex_count = lod_data.vertices.size();
                level.index_count = lod_data.indices.size();
                level.gpu_mesh = render_api->createMesh();
                if (level.gpu_mesh)
                {
                    level.gpu_mesh->uploadIndexedMeshData(
                        lod_data.vertices.data(), lod_data.vertices.size(),
                        lod_data.indices.data(), lod_data.indices.size()
                    );
                }

                // Map LOD submesh ranges to original mesh's material textures
                if (!lod_data.submesh_ranges.empty() && m.uses_material_ranges)
                {
                    for (const auto& sr : lod_data.submesh_ranges)
                    {
                        TextureHandle tex = INVALID_TEXTURE;
                        std::string mat_name = "";
                        if (sr.submesh_id < m.material_ranges.size())
                        {
                            tex = m.material_ranges[sr.submesh_id].texture;
                            mat_name = m.material_ranges[sr.submesh_id].material_name;
                        }
                        level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
                    }
                }

                m.lod_levels.push_back(std::move(level));
            }
        }

        if (!m.lod_levels.empty())
            m.computeBounds();
    }
}

void EditorApp::shutdown()
{
    // Stop simulation if running
    if (m_state.isSimulationActive())
        stopPlay();

    // Save window geometry to config
    if (m_app.getWindow())
    {
        Uint32 flags = SDL_GetWindowFlags(m_app.getWindow());
        bool is_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;

        if (auto* cvar = CVAR_PTR(window_maximized))
            cvar->setBool(is_maximized);

        if (!is_maximized)
        {
            if (auto* cvar = CVAR_PTR(window_width))
                cvar->setInt(m_app.getWidth());
            if (auto* cvar = CVAR_PTR(window_height))
                cvar->setInt(m_app.getHeight());
        }

        ConVarRegistry::get().saveArchiveCvars("config.cfg");
    }

    if (m_editor_config)
        m_editor_config->save();

    if (auto* api = m_app.getRenderAPI())
        api->waitForGPU();
    m_world.registry.clear();
    Console::get().shutdown();
    RmlUiManager::get().shutdown();
    ImGuiManager::get().shutdown();
    m_app.shutdown();
    EE::CLog::Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Play In Editor (PIE) state transitions
// ─────────────────────────────────────────────────────────────────────────────

void EditorApp::beginPlay()
{
    if (m_state.isSimulationActive())
        return; // already playing

    // Guard: nothing to play
    auto view = m_world.registry.view<TagComponent, TransformComponent>();
    if (view.size_hint() == 0)
    {
        LOG_ENGINE_WARN("Cannot play: no entities in the level");
        return;
    }

    LOG_ENGINE_INFO("--- PIE: Starting play mode ---");

    // Clear console if requested
    if (m_console.shouldClearOnPlay())
        Console::get().clear();

    // 1. Snapshot current state
    m_play_snapshot = buildLevelDataFromECS();
    m_pre_play_editor_cam = m_editor_cam.cam;

    // Save selected entity name for restoration
    m_pre_play_selected_name.clear();
    if (m_hierarchy.selected_entity != entt::null &&
        m_world.registry.valid(m_hierarchy.selected_entity) &&
        m_world.registry.all_of<TagComponent>(m_hierarchy.selected_entity))
    {
        m_pre_play_selected_name = m_world.registry.get<TagComponent>(m_hierarchy.selected_entity).name;
    }

    // Determine if we should use the game DLL for network PIE
    bool use_network_pie = (m_state.network_pie.net_mode != PIENetMode::Standalone);
    std::string dll_path = m_project_manager.getAbsoluteModulePath();

    if (use_network_pie && (dll_path.empty() || !std::filesystem::exists(dll_path)))
    {
        LOG_ENGINE_WARN("Network PIE requested but no game module found — falling back to Standalone");
        use_network_pie = false;
    }

    if (use_network_pie)
    {
        // ---- Network PIE path: load game DLL, run server + client ----

        // Load game DLL
        if (!m_game_module.load(dll_path))
        {
            LOG_ENGINE_ERROR("Failed to load game module '{}' — falling back to Standalone", dll_path);
            use_network_pie = false;
        }
    }

    if (use_network_pie && m_state.network_pie.net_mode == PIENetMode::ListenServer)
    {
        // --- Listen Server: server + client in-process ---

        if (!m_game_module.hasServerSupport())
        {
            LOG_ENGINE_WARN("Game module has no server support — falling back to Standalone");
            m_game_module.unload();
            use_network_pie = false;
        }
        else
        {
            uint16_t port = m_state.network_pie.server_port;

            // Initialize a separate server world
            m_server_world = world();
            m_server_world.initializePhysics();

            // Instantiate level into server world
            m_level_manager.instantiateLevel(m_play_snapshot, m_server_world,
                m_app.getRenderAPI(), nullptr, nullptr, nullptr);

            // Set up server EngineServices
            m_server_services = {};
            m_server_services.game_world    = &m_server_world;
            m_server_services.render_api    = m_app.getRenderAPI();
            m_server_services.input_manager = nullptr;
            m_server_services.reflection    = &m_reflection;
            m_server_services.application   = &m_app;
            m_server_services.level_manager = &m_level_manager;
            m_server_services.api_version   = GARDEN_MODULE_API_VERSION;
            m_server_services.listen_port   = port;

            m_game_module.registerComponents(&m_reflection);

            if (!m_game_module.serverInit(&m_server_services))
            {
                LOG_ENGINE_ERROR("Server initialization failed — falling back to Standalone");
                m_server_world.resetWorld();
                m_game_module.unload();
                use_network_pie = false;
            }
            else
            {
                m_game_module.serverOnLevelLoaded();

                // Create input manager for client
                m_game_input_manager = std::make_shared<InputManager>();

                // Set up client EngineServices
                m_client_services = {};
                m_client_services.game_world      = &m_world;
                m_client_services.render_api       = m_app.getRenderAPI();
                m_client_services.input_manager    = m_game_input_manager.get();
                m_client_services.reflection       = &m_reflection;
                m_client_services.application      = &m_app;
                m_client_services.level_manager    = &m_level_manager;
                m_client_services.api_version      = GARDEN_MODULE_API_VERSION;
                m_client_services.connect_address  = "127.0.0.1";
                m_client_services.connect_port     = port;

                if (!m_game_module.init(&m_client_services))
                {
                    LOG_ENGINE_ERROR("Client initialization failed");
                    m_game_module.serverShutdown();
                    m_server_world.resetWorld();
                    m_game_module.unload();
                    use_network_pie = false;
                }
                else
                {
                    m_game_module.onLevelLoaded();
                    m_network_pie_active = true;

                    // Launch additional players based on run mode
                    if (m_state.network_pie.num_players > 1)
                    {
                        if (m_state.network_pie.run_mode == PIERunMode::InEditor)
                        {
                            // In-Editor mode: load DLL copies for each additional client
                            for (int i = 2; i <= m_state.network_pie.num_players; i++)
                            {
                                auto inst = std::make_unique<PIEClientInstance>();
                                inst->player_index = i;
                                inst->window_title = "Player " + std::to_string(i);

                                // Create isolated world
                                inst->client_world = world();
                                inst->client_world.initializePhysics();
                                m_level_manager.instantiateLevel(m_play_snapshot, inst->client_world,
                                    m_app.getRenderAPI(), nullptr, nullptr, nullptr);

                                // Load a separate DLL copy (hot-reload mechanism gives us isolation)
                                if (!inst->game_module.load(dll_path))
                                {
                                    LOG_ENGINE_WARN("Failed to load DLL copy for Player {}", i);
                                    inst->client_world.resetWorld();
                                    continue;
                                }

                                // Create render target
                                inst->viewport_id = m_app.getRenderAPI()->createPIEViewport(640, 480);
                                if (inst->viewport_id < 0)
                                {
                                    LOG_ENGINE_WARN("Failed to create PIE viewport for Player {}", i);
                                    inst->game_module.unload();
                                    inst->client_world.resetWorld();
                                    continue;
                                }

                                // Set up input and services
                                inst->input_manager = std::make_shared<InputManager>();
                                inst->services = {};
                                inst->services.game_world      = &inst->client_world;
                                inst->services.render_api       = m_app.getRenderAPI();
                                inst->services.input_manager    = inst->input_manager.get();
                                inst->services.reflection       = &m_reflection;
                                inst->services.application      = &m_app;
                                inst->services.level_manager    = &m_level_manager;
                                inst->services.api_version      = GARDEN_MODULE_API_VERSION;
                                inst->services.connect_address  = "127.0.0.1";
                                inst->services.connect_port     = port;

                                inst->game_module.registerComponents(&m_reflection);
                                if (!inst->game_module.init(&inst->services))
                                {
                                    LOG_ENGINE_WARN("Client init failed for Player {}", i);
                                    m_app.getRenderAPI()->destroyPIEViewport(inst->viewport_id);
                                    inst->game_module.unload();
                                    inst->client_world.resetWorld();
                                    continue;
                                }

                                inst->game_module.onLevelLoaded();
                                inst->initialized = true;
                                LOG_ENGINE_INFO("PIE: Player {} initialized in-editor", i);

                                m_pie_clients.push_back(std::move(inst));
                            }
                        }
                        else
                        {
                            // Separate Windows mode: spawn Game.exe for each additional player
                            std::filesystem::path exe_dir = EnginePaths::getExecutableDir();
                            std::string game_exe = (exe_dir / PIE_GAME_EXE_NAME).string();
                            std::string project_path = m_project_manager.getProjectFilePath();

                            for (int i = 2; i <= m_state.network_pie.num_players; i++)
                            {
                                if (!m_pie_processes.spawnClient(i, game_exe, project_path, "127.0.0.1", port))
                                    LOG_ENGINE_WARN("Failed to spawn Player {}", i);
                            }
                        }
                    }

                    LOG_ENGINE_INFO("--- PIE: Listen Server started on port {} ---", port);
                }
            }
        }
    }
    else if (use_network_pie && m_state.network_pie.net_mode == PIENetMode::DedicatedServer)
    {
        // --- Dedicated Server: spawn Server.exe, editor runs as client ---

        uint16_t port = m_state.network_pie.server_port;
        std::filesystem::path exe_dir = EnginePaths::getExecutableDir();
        std::string server_exe = (exe_dir / PIE_SERVER_EXE_NAME).string();
        std::string game_exe = (exe_dir / PIE_GAME_EXE_NAME).string();
        std::string project_path = m_project_manager.getProjectFilePath();

        // Spawn dedicated server process
        if (!m_pie_processes.spawnServer(server_exe, project_path, port))
        {
            LOG_ENGINE_ERROR("Failed to spawn dedicated server — falling back to Standalone");
            m_game_module.unload();
            use_network_pie = false;
        }
        else
        {
            // Give server a brief moment to start listening
            SDL_Delay(500);

            // Create input manager for client
            m_game_input_manager = std::make_shared<InputManager>();

            // Register components
            m_game_module.registerComponents(&m_reflection);

            // Set up client EngineServices
            m_client_services = {};
            m_client_services.game_world      = &m_world;
            m_client_services.render_api       = m_app.getRenderAPI();
            m_client_services.input_manager    = m_game_input_manager.get();
            m_client_services.reflection       = &m_reflection;
            m_client_services.application      = &m_app;
            m_client_services.level_manager    = &m_level_manager;
            m_client_services.api_version      = GARDEN_MODULE_API_VERSION;
            m_client_services.connect_address  = "127.0.0.1";
            m_client_services.connect_port     = port;

            if (!m_game_module.init(&m_client_services))
            {
                LOG_ENGINE_ERROR("Client initialization failed");
                m_pie_processes.killAll();
                m_game_module.unload();
                use_network_pie = false;
            }
            else
            {
                m_game_module.onLevelLoaded();
                m_network_pie_active = true;

                // Launch additional players based on run mode
                if (m_state.network_pie.num_players > 1)
                {
                    if (m_state.network_pie.run_mode == PIERunMode::InEditor)
                    {
                        for (int i = 2; i <= m_state.network_pie.num_players; i++)
                        {
                            auto inst = std::make_unique<PIEClientInstance>();
                            inst->player_index = i;
                            inst->window_title = "Player " + std::to_string(i);
                            inst->client_world = world();
                            inst->client_world.initializePhysics();
                            m_level_manager.instantiateLevel(m_play_snapshot, inst->client_world,
                                m_app.getRenderAPI(), nullptr, nullptr, nullptr);

                            if (!inst->game_module.load(dll_path))
                            {
                                LOG_ENGINE_WARN("Failed to load DLL copy for Player {}", i);
                                inst->client_world.resetWorld();
                                continue;
                            }

                            inst->viewport_id = m_app.getRenderAPI()->createPIEViewport(640, 480);
                            if (inst->viewport_id < 0)
                            {
                                LOG_ENGINE_WARN("Failed to create PIE viewport for Player {}", i);
                                inst->game_module.unload();
                                inst->client_world.resetWorld();
                                continue;
                            }

                            inst->input_manager = std::make_shared<InputManager>();
                            inst->services = {};
                            inst->services.game_world      = &inst->client_world;
                            inst->services.render_api       = m_app.getRenderAPI();
                            inst->services.input_manager    = inst->input_manager.get();
                            inst->services.reflection       = &m_reflection;
                            inst->services.application      = &m_app;
                            inst->services.level_manager    = &m_level_manager;
                            inst->services.api_version      = GARDEN_MODULE_API_VERSION;
                            inst->services.connect_address  = "127.0.0.1";
                            inst->services.connect_port     = port;

                            inst->game_module.registerComponents(&m_reflection);
                            if (!inst->game_module.init(&inst->services))
                            {
                                LOG_ENGINE_WARN("Client init failed for Player {}", i);
                                m_app.getRenderAPI()->destroyPIEViewport(inst->viewport_id);
                                inst->game_module.unload();
                                inst->client_world.resetWorld();
                                continue;
                            }

                            inst->game_module.onLevelLoaded();
                            inst->initialized = true;
                            LOG_ENGINE_INFO("PIE: Player {} initialized in-editor", i);
                            m_pie_clients.push_back(std::move(inst));
                        }
                    }
                    else
                    {
                        for (int i = 2; i <= m_state.network_pie.num_players; i++)
                        {
                            if (!m_pie_processes.spawnClient(i, game_exe, project_path, "127.0.0.1", port))
                                LOG_ENGINE_WARN("Failed to spawn Player {}", i);
                        }
                    }
                }

                LOG_ENGINE_INFO("--- PIE: Dedicated Server mode on port {} ---", port);
            }
        }
    }

    if (!use_network_pie && !m_network_pie_active)
    {
        // ---- Standalone path: existing GameSimulation ----
        m_game_input_manager = std::make_shared<InputManager>();
        m_game_sim = std::make_unique<GameSimulation>(&m_world, m_game_input_manager);
        m_game_sim->initialize();
    }

    // Enter playing state (mouse stays free until user clicks viewport)
    m_state.play_mode = PlayMode::Playing;
    m_mouse_captured_for_game = false;

    m_renderer.markBVHDirty();

    LOG_ENGINE_INFO("--- PIE: Play mode started (click viewport to capture mouse) ---");
}

void EditorApp::stopPlay()
{
    if (!m_state.isSimulationActive())
        return; // not playing

    LOG_ENGINE_INFO("--- PIE: Stopping play mode ---");

    // 1. Tear down network PIE or standalone simulation
    if (m_network_pie_active)
    {
        // Kill spawned processes first (clients disconnect before server shuts down)
        m_pie_processes.killAll();

        // Tear down additional in-editor PIE client instances
        for (auto& inst : m_pie_clients)
        {
            if (inst && inst->initialized)
            {
                inst->game_module.shutdown();
                inst->game_module.unload();
                if (inst->viewport_id >= 0)
                    m_app.getRenderAPI()->destroyPIEViewport(inst->viewport_id);
                inst->client_world.resetWorld();
                inst->initialized = false;
            }
        }
        m_pie_clients.clear();
        m_focused_pie_client = -1;

        // Shutdown Player 1 client
        m_game_module.shutdown();

        // Shutdown server (only for listen server — dedicated server was a separate process)
        if (m_state.network_pie.net_mode == PIENetMode::ListenServer)
        {
            m_game_module.serverShutdown();
            m_server_world.resetWorld();
        }

        m_game_module.unload();
        m_game_input_manager.reset();
        m_network_pie_active = false;

        LOG_ENGINE_INFO("Network PIE shutdown complete");
    }
    else
    {
        m_game_sim.reset();
        m_game_input_manager.reset();
    }

    // 2. Reset world (shutdown physics, clear registry, reinitialize)
    m_world.resetWorld();
    m_level_manager.cleanup();
    m_inspector.mesh_path_cache.clear();
    m_hierarchy.selected_entity = entt::null;

    // 3. Restore from snapshot
    m_level_data = m_play_snapshot;
    m_level_settings.metadata = &m_level_data.metadata;

    m_level_manager.instantiateLevel(
        m_play_snapshot, m_world, m_app.getRenderAPI(),
        nullptr, nullptr, nullptr);

    // 4. Rebuild caches
    buildMeshPathCache();

    // 5. Restore editor camera
    m_editor_cam.cam = m_pre_play_editor_cam;

    // 6. Restore selection by name (best-effort)
    if (!m_pre_play_selected_name.empty())
    {
        auto tag_view = m_world.registry.view<TagComponent>();
        for (auto entity : tag_view)
        {
            if (tag_view.get<TagComponent>(entity).name == m_pre_play_selected_name)
            {
                m_hierarchy.selected_entity = entity;
                break;
            }
        }
    }

    // 7. Restore state
    m_state.play_mode = PlayMode::Editing;
    m_mouse_captured_for_game = false;
    SDL_SetRelativeMouseMode(SDL_FALSE);

    applyLightingFromMetadata();
    m_renderer.markBVHDirty();

    LOG_ENGINE_INFO("--- PIE: Play mode stopped, state restored ---");
}

void EditorApp::pausePlay()
{
    if (m_state.play_mode != PlayMode::Playing)
        return;

    if (m_network_pie_active)
    {
        // Network PIE: can't truly pause networking, but stop ticking locally
        LOG_ENGINE_INFO("PIE: Paused (network connections remain active)");
    }
    else if (m_game_sim)
    {
        m_game_sim->setPaused(true);
    }

    m_state.play_mode = PlayMode::Paused;
    m_mouse_captured_for_game = false;
    SDL_SetRelativeMouseMode(SDL_FALSE);

    LOG_ENGINE_INFO("PIE: Paused");
}

void EditorApp::resumePlay()
{
    if (m_state.play_mode != PlayMode::Paused)
        return;

    if (!m_network_pie_active && m_game_sim)
        m_game_sim->setPaused(false);

    m_state.play_mode = PlayMode::Playing;
    // Don't auto-capture mouse, user clicks viewport to recapture

    LOG_ENGINE_INFO("PIE: Resumed");
}

void EditorApp::ejectFromPlay()
{
    if (m_state.play_mode != PlayMode::Playing)
        return;

    m_state.play_mode = PlayMode::Ejected;
    m_mouse_captured_for_game = false;
    SDL_SetRelativeMouseMode(SDL_FALSE);

    // Sync editor camera to current game camera so user starts flying from there
    if (m_game_sim)
        m_editor_cam.cam = m_game_sim->getActiveCamera();

    LOG_ENGINE_INFO("PIE: Ejected (editor camera, simulation continues)");
}

void EditorApp::returnToPlay()
{
    if (m_state.play_mode != PlayMode::Ejected)
        return;

    m_state.play_mode = PlayMode::Playing;
    // Don't auto-capture mouse, user clicks viewport to recapture

    LOG_ENGINE_INFO("PIE: Returned to play");
}

camera& EditorApp::chooseRenderCamera()
{
    switch (m_state.play_mode)
    {
    case PlayMode::Playing:
    case PlayMode::Paused:
        if (m_network_pie_active)
            return m_world.world_camera; // game DLL updates world_camera
        if (m_game_sim)
            return m_game_sim->getActiveCamera();
        return m_editor_cam.cam;

    case PlayMode::Ejected:
        return m_editor_cam.cam;

    case PlayMode::Editing:
    default:
        return m_editor_cam.cam;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Event processing with input routing
// ─────────────────────────────────────────────────────────────────────────────

void EditorApp::processEvents()
{
    // Update game input manager at start of frame (resets deltas)
    if (m_game_input_manager)
        m_game_input_manager->update();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        ImGuiManager::get().processEvent(&event);

        switch (event.type)
        {
        case SDL_QUIT:
            if (m_state.isSimulationActive())
                stopPlay();
            m_running = false;
            break;

        case SDL_KEYDOWN:
            if (!event.key.repeat)
            {
                // --- Global hotkeys (work in any mode) ---

                // F1: toggle UI
                if (event.key.keysym.scancode == SDL_SCANCODE_F1)
                {
                    m_show_ui = !m_show_ui;
                    break;
                }

                // Escape during play: first release mouse, second press stops play
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
                    m_state.isSimulationActive())
                {
                    if (m_mouse_captured_for_game)
                    {
                        // First Escape: release mouse capture
                        m_mouse_captured_for_game = false;
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                    }
                    else
                    {
                        // Second Escape (or Escape when not captured): stop play
                        stopPlay();
                    }
                    break;
                }

                // Escape in editing mode: deselect selected entity
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
                    !m_state.isSimulationActive() &&
                    !ImGui::GetIO().WantTextInput)
                {
                    if (m_hierarchy.selected_entity != entt::null)
                        m_hierarchy.selected_entity = entt::null;
                    break;
                }

                // F8: eject/return toggle during play
                if (event.key.keysym.scancode == SDL_SCANCODE_F8)
                {
                    if (m_state.play_mode == PlayMode::Playing)
                    {
                        ejectFromPlay();
                        break;
                    }
                    else if (m_state.play_mode == PlayMode::Ejected)
                    {
                        returnToPlay();
                        break;
                    }
                }

                // Ctrl+S: save (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_S &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !m_state.isSimulationActive())
                {
                    saveLevel();
                    break;
                }

                // Ctrl+N: new level (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_N &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !m_state.isSimulationActive())
                {
                    newLevel();
                    break;
                }

                // Ctrl+C: copy selected entity (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_C &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !m_state.isSimulationActive())
                {
                    copySelectedEntity();
                    break;
                }

                // Ctrl+V: paste entity (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_V &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !m_state.isSimulationActive())
                {
                    pasteEntity();
                    break;
                }

                // Ctrl+D: duplicate selected entity (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_D &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !m_state.isSimulationActive())
                {
                    if (m_hierarchy.selected_entity != entt::null &&
                        m_world.registry.valid(m_hierarchy.selected_entity))
                    {
                        m_undo.pushState(buildLevelDataFromECS(), "duplicate entity");
                        m_hierarchy.duplicateEntity(m_world.registry, m_hierarchy.selected_entity);
                        m_renderer.markBVHDirty();
                        m_state.unsaved_changes = true;
                    }
                    break;
                }

                // Ctrl+Z: undo (only in editing mode)
                if (event.key.keysym.scancode == SDL_SCANCODE_Z &&
                    (SDL_GetModState() & KMOD_CTRL) &&
                    !(SDL_GetModState() & KMOD_SHIFT) &&
                    !m_state.isSimulationActive())
                {
                    if (m_undo.canUndo())
                    {
                        const LevelData& snapshot = m_undo.undo();
                        restoreFromSnapshot(snapshot);
                    }
                    break;
                }

                // Ctrl+Y / Ctrl+Shift+Z: redo (only in editing mode)
                if (((event.key.keysym.scancode == SDL_SCANCODE_Y && (SDL_GetModState() & KMOD_CTRL)) ||
                     (event.key.keysym.scancode == SDL_SCANCODE_Z && (SDL_GetModState() & KMOD_CTRL) && (SDL_GetModState() & KMOD_SHIFT))) &&
                    !m_state.isSimulationActive())
                {
                    if (m_undo.canRedo())
                    {
                        const LevelData& snapshot = m_undo.redo();
                        restoreFromSnapshot(snapshot);
                    }
                    break;
                }

                // --- Editor-only hotkeys (transform mode, delete, focus) ---
                if (!m_state.isSimulationActive() || m_state.play_mode == PlayMode::Ejected)
                {
                    if (!ImGui::GetIO().WantTextInput && !m_state.gizmo_using)
                    {
                        if (event.key.keysym.scancode == SDL_SCANCODE_W)
                            m_state.transform_mode = EditorState::TransformMode::Translate;
                        if (event.key.keysym.scancode == SDL_SCANCODE_E)
                            m_state.transform_mode = EditorState::TransformMode::Rotate;
                        if (event.key.keysym.scancode == SDL_SCANCODE_R)
                            m_state.transform_mode = EditorState::TransformMode::Scale;

                        // Delete: delete selected entity
                        if (event.key.keysym.scancode == SDL_SCANCODE_DELETE)
                        {
                            if (m_hierarchy.selected_entity != entt::null &&
                                m_world.registry.valid(m_hierarchy.selected_entity))
                            {
                                m_undo.pushState(buildLevelDataFromECS(), "delete entity");
                                m_world.registry.destroy(m_hierarchy.selected_entity);
                                m_hierarchy.selected_entity = entt::null;
                                m_renderer.markBVHDirty();
                                m_state.unsaved_changes = true;
                            }
                        }

                        // F: focus/frame selected entity
                        if (event.key.keysym.scancode == SDL_SCANCODE_F)
                        {
                            if (m_hierarchy.selected_entity != entt::null &&
                                m_world.registry.valid(m_hierarchy.selected_entity))
                            {
                                auto* t = m_world.registry.try_get<TransformComponent>(m_hierarchy.selected_entity);
                                if (t)
                                {
                                    glm::vec3 center = t->position;
                                    float distance = 5.0f;

                                    auto* mc = m_world.registry.try_get<MeshComponent>(m_hierarchy.selected_entity);
                                    if (mc && mc->m_mesh && mc->m_mesh->bounds_computed)
                                    {
                                        glm::vec3 extents = (mc->m_mesh->aabb_max - mc->m_mesh->aabb_min) * t->scale;
                                        distance = glm::length(extents) * 1.5f;
                                        if (distance < 2.0f) distance = 2.0f;
                                        center = t->position + (mc->m_mesh->aabb_min + mc->m_mesh->aabb_max) * 0.5f * t->scale;
                                    }

                                    m_editor_cam.cam.position = center - m_editor_cam.cam.camera_forward() * distance;
                                }
                            }
                        }
                    }
                }
            }

            // Route keyboard to game input during Playing mode
            if (m_state.play_mode == PlayMode::Playing && m_game_input_manager &&
                (m_mouse_captured_for_game || !ImGui::GetIO().WantCaptureKeyboard))
            {
                m_game_input_manager->process_event(event);
            }
            break;

        case SDL_KEYUP:
            // Route key release to game input during Playing mode
            if (m_state.play_mode == PlayMode::Playing && m_game_input_manager &&
                (m_mouse_captured_for_game || !ImGui::GetIO().WantCaptureKeyboard))
            {
                m_game_input_manager->process_event(event);
            }
            break;

        case SDL_MOUSEMOTION:
            if (m_mouse_captured_for_game && m_game_input_manager)
            {
                // Route mouse motion to game input when captured
                m_game_input_manager->process_event(event);
            }
            else
            {
                // Editor camera motion accumulation
                m_mouse_dx += static_cast<float>(event.motion.xrel);
                m_mouse_dy += static_cast<float>(event.motion.yrel);
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            // During Playing mode: left-click in viewport captures mouse for game
            if (m_state.play_mode == PlayMode::Playing && !m_mouse_captured_for_game)
            {
                if (event.button.button == SDL_BUTTON_LEFT &&
                    m_viewport.is_hovered)
                {
                    m_mouse_captured_for_game = true;
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                    break;
                }
            }

            if (m_mouse_captured_for_game && m_game_input_manager)
            {
                m_game_input_manager->process_event(event);
            }
            else if (!m_state.isSimulationActive() || m_state.play_mode == PlayMode::Ejected)
            {
                // Editor camera: right-click to fly (blocked during gizmo drag)
                if (event.button.button == SDL_BUTTON_RIGHT && !m_state.gizmo_using)
                {
                    m_right_mouse = true;
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (m_mouse_captured_for_game && m_game_input_manager)
            {
                m_game_input_manager->process_event(event);
            }
            else if (!m_state.isSimulationActive() || m_state.play_mode == PlayMode::Ejected)
            {
                if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    m_right_mouse = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
            }
            break;

        case SDL_MOUSEWHEEL:
            // Scroll wheel adjusts editor camera speed while in fly mode (RMB held)
            if (m_right_mouse && event.wheel.y != 0)
            {
                m_editor_cam.adjustSpeed(static_cast<float>(event.wheel.y));
            }
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                m_app.onWindowResized(event.window.data1, event.window.data2);
            }
            break;

        default:
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Dockspace, menus, dialogs (mostly unchanged, with play-mode guards)
// ─────────────────────────────────────────────────────────────────────────────

void EditorApp::renderDockspace()
{
    ImGuiWindowFlags dockspace_flags =
        ImGuiWindowFlags_MenuBar        |
        ImGuiWindowFlags_NoDocking      |
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoCollapse     |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Account for status bar at bottom
    float status_bar_height = m_show_status_bar ? (ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f) : 0.0f;

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - status_bar_height));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));

    ImGui::Begin("##DockSpaceWindow", nullptr, dockspace_flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpaceV2");

    // Set up default dock layout only when no saved layout exists
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
    {
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(viewport->Size.x, viewport->Size.y - status_bar_height));

        ImGuiID dock_main = dockspace_id;

        // Split bottom panel (25% height)
        ImGuiID dock_bottom;
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.25f, &dock_bottom, &dock_main);

        // Split left panel (20% width)
        ImGuiID dock_left;
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.2f, &dock_left, &dock_main);

        // Split right panel (25% width of remaining)
        ImGuiID dock_right;
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.25f, &dock_right, &dock_main);

        // Dock windows to their regions
        ImGui::DockBuilderDockWindow("Viewport", dock_main);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left);
        ImGui::DockBuilderDockWindow("Inspector", dock_right);
        ImGui::DockBuilderDockWindow("Level Settings", dock_right);
        ImGui::DockBuilderDockWindow("Console", dock_bottom);
        ImGui::DockBuilderDockWindow("Content Browser", dock_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));

    renderMenuBar();

    ImGui::End();
}

void EditorApp::renderMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            bool can_edit = !m_state.isSimulationActive();

            if (ImGui::MenuItem("New", nullptr, false, can_edit))
                newLevel();

            if (ImGui::MenuItem("Open Level...", nullptr, false, can_edit))
            {
                std::string path = FileDialog::openFile("Open Level",
                    "Level Files (*.level.json)\0*.level.json\0All Files (*.*)\0*.*\0");
                if (!path.empty())
                    openLevel(path);
            }

            if (ImGui::MenuItem("Save", "Ctrl+S", false, can_edit))
                saveLevel();

            if (ImGui::MenuItem("Save As...", nullptr, false, can_edit))
            {
                std::string path = FileDialog::saveFile("Save Level As",
                    "Level Files (*.level.json)\0*.level.json\0All Files (*.*)\0*.*\0");
                if (!path.empty())
                    saveLevelAs(path);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Package Project...", nullptr, false,
                can_edit && m_project_manager.isLoaded()))
            {
                std::strncpy(m_package_name,
                    m_project_manager.getDescriptor().name.c_str(),
                    sizeof(m_package_name) - 1);
                m_package_name[sizeof(m_package_name) - 1] = '\0';

                // Load last output directory from ConVar if empty
                if (m_package_output_dir[0] == '\0')
                {
                    std::string last_dir = CVAR_STRING(editor_package_output_dir);
                    if (!last_dir.empty())
                        std::strncpy(m_package_output_dir, last_dir.c_str(), sizeof(m_package_output_dir) - 1);
                }

                m_package_phase = PackagePhase::Configure;
                m_package_result = {};
                m_package_pre_warnings = ProjectPackager::validateBeforePackage(
                    m_project_manager, PackageConfig{m_package_output_dir, m_package_name,
                        m_package_compile_levels, m_app.getAPIType()});
                m_show_package_dialog = true;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Quit"))
            {
                if (m_state.isSimulationActive())
                    stopPlay();
                m_running = false;
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            bool can_edit = !m_state.isSimulationActive();

            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, can_edit && m_undo.canUndo()))
            {
                const LevelData& snapshot = m_undo.undo();
                restoreFromSnapshot(snapshot);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, can_edit && m_undo.canRedo()))
            {
                const LevelData& snapshot = m_undo.redo();
                restoreFromSnapshot(snapshot);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Copy", "Ctrl+C", false,
                can_edit && m_hierarchy.selected_entity != entt::null))
            {
                copySelectedEntity();
            }

            if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                can_edit && m_entity_clipboard.has_value()))
            {
                pasteEntity();
            }

            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false,
                can_edit && m_hierarchy.selected_entity != entt::null))
            {
                m_undo.pushState(buildLevelDataFromECS(), "duplicate entity");
                m_hierarchy.duplicateEntity(m_world.registry, m_hierarchy.selected_entity);
                m_renderer.markBVHDirty();
                m_state.unsaved_changes = true;
            }

            if (ImGui::MenuItem("Delete", "Del", false,
                can_edit && m_hierarchy.selected_entity != entt::null))
            {
                m_undo.pushState(buildLevelDataFromECS(), "delete entity");
                m_world.registry.destroy(m_hierarchy.selected_entity);
                m_hierarchy.selected_entity = entt::null;
                m_renderer.markBVHDirty();
                m_state.unsaved_changes = true;
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Viewport",        nullptr, &m_show_viewport);
            ImGui::MenuItem("Toolbar",         nullptr, &m_show_toolbar);
            ImGui::MenuItem("Hierarchy",       nullptr, &m_show_hierarchy);
            ImGui::MenuItem("Inspector",       nullptr, &m_show_inspector);
            ImGui::MenuItem("Level Settings",  nullptr, &m_show_level_settings);
            ImGui::MenuItem("Console",         nullptr, &m_show_console);
            ImGui::MenuItem("Content Browser", nullptr, &m_show_content_browser);
            ImGui::MenuItem("Asset Preview",   nullptr, &m_show_model_preview);
            ImGui::MenuItem("Status Bar",      nullptr, &m_show_status_bar);
            ImGui::MenuItem("NavMesh",         nullptr, &m_show_navmesh_panel);
            ImGui::MenuItem("Physics Debug",   nullptr, &m_show_physics_debug);
            ImGui::Separator();
            ImGui::MenuItem("Viewport Stats",  nullptr, &m_state.show_viewport_stats);
            ImGui::MenuItem("Grid",            nullptr, &m_state.show_grid);
            ImGui::Separator();
            ImGui::MenuItem("All Panels (F1)", nullptr, &m_show_ui);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings"))
        {
            if (ImGui::MenuItem("Editor Settings..."))
                m_show_editor_settings = true;
            ImGui::EndMenu();
        }

        // Display current file in the menu bar (with dirty indicator)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20.0f);
        if (m_current_save_path.empty())
            ImGui::TextDisabled(m_state.unsaved_changes ? "* (unsaved)" : "(unsaved)");
        else
            ImGui::TextDisabled(m_state.unsaved_changes ? "* %s" : "%s", m_current_save_path.c_str());

        ImGui::EndMenuBar();
    }
}

void EditorApp::renderOpenDialog()
{
    if (!m_show_open_dialog) return;

    ImGui::SetNextWindowSize(ImVec2(500.0f, 110.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Open Level##dialog", &m_show_open_dialog,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        ImGui::Text("Level path:");
        ImGui::SetNextItemWidth(-1.0f);
        bool enter = ImGui::InputText("##open_path", m_open_path_buf, sizeof(m_open_path_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::Button("Open", ImVec2(80.0f, 0.0f)) || enter)
        {
            openLevel(std::string(m_open_path_buf));
            m_show_open_dialog = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
            m_show_open_dialog = false;
    }
    ImGui::End();
}

void EditorApp::renderEditorSettings()
{
    if (!m_show_editor_settings || !m_editor_config) return;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Editor Settings", &m_show_editor_settings,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SeparatorText("Graphics");

        auto backends = EditorConfig::availableBackends();
        int current_idx = 0;
        for (int i = 0; i < (int)backends.size(); i++)
        {
            if (backends[i] == m_editor_config->render_backend)
                current_idx = i;
        }

        ImGui::Text("Render Backend");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##render_backend", EditorConfig::backendDisplayName(backends[current_idx])))
        {
            for (int i = 0; i < (int)backends.size(); i++)
            {
                bool selected = (i == current_idx);
                if (ImGui::Selectable(EditorConfig::backendDisplayName(backends[i]), selected))
                {
                    m_editor_config->render_backend = backends[i];
                    m_editor_config->save();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (m_editor_config->render_backend != m_app.getAPIType())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                "Restart the editor for the backend change to take effect.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Rendering Features");

        // FXAA toggle
        {
            auto* cvar = CVAR_PTR(r_fxaa);
            bool fxaa = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("FXAA Anti-aliasing", &fxaa))
            {
                if (cvar) cvar->setInt(fxaa ? 1 : 0);
                m_app.getRenderAPI()->setFXAAEnabled(fxaa);
            }
        }

        // Shadow Quality combo
        {
            auto* cvar = CVAR_PTR(r_shadowquality);
            int shadow_q = cvar ? cvar->getInt() : 2;
            const char* shadow_opts[] = { "Off", "Low (1024)", "Medium (2048)", "High (4096)" };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Shadow Quality", &shadow_q, shadow_opts, 4))
            {
                if (cvar) cvar->setInt(shadow_q);
                m_app.getRenderAPI()->setShadowQuality(shadow_q);
            }
        }

        // Skybox toggle
        {
            auto* cvar = CVAR_PTR(r_sky);
            bool sky = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("Skybox", &sky))
            {
                if (cvar) cvar->setInt(sky ? 1 : 0);
            }
        }

        // Lighting toggle
        {
            auto* cvar = CVAR_PTR(r_lighting);
            bool lighting = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("Lighting", &lighting))
            {
                if (cvar) cvar->setInt(lighting ? 1 : 0);
                m_app.getRenderAPI()->enableLighting(lighting);
            }
            if (!lighting)
                ImGui::TextDisabled("  All objects render unlit (flat color).");
        }

        // Dynamic Lights toggle
        {
            auto* cvar = CVAR_PTR(r_dynamiclights);
            bool dyn = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("Dynamic Lights (Point/Spot)", &dyn))
            {
                if (cvar) cvar->setInt(dyn ? 1 : 0);
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Performance");

        // Depth Prepass toggle
        {
            auto* cvar = CVAR_PTR(r_depthprepass);
            bool prepass = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("Depth Prepass", &prepass))
            {
                if (cvar) cvar->setInt(prepass ? 1 : 0);
                m_renderer.setDepthPrepassEnabled(prepass);
            }
        }

        // Frustum Culling / BVH toggle
        {
            auto* cvar = CVAR_PTR(r_frustumculling);
            bool culling = cvar ? cvar->getBool() : true;
            if (ImGui::Checkbox("Frustum Culling (BVH)", &culling))
            {
                if (cvar) cvar->setInt(culling ? 1 : 0);
                m_renderer.setBVHEnabled(culling);
            }
        }

        // Reset to Defaults
        ImGui::Spacing();
        if (ImGui::Button("Reset to Defaults"))
        {
            const char* cvar_names[] = { "r_fxaa", "r_shadowquality", "r_sky", "r_lighting",
                                          "r_dynamiclights", "r_depthprepass", "r_frustumculling" };
            for (const char* name : cvar_names)
            {
                if (auto* cv = ConVarRegistry::get().find(name))
                    cv->reset();
            }
            m_app.getRenderAPI()->setFXAAEnabled(CVAR_BOOL(r_fxaa));
            m_app.getRenderAPI()->setShadowQuality(CVAR_INT(r_shadowquality));
            m_app.getRenderAPI()->enableLighting(CVAR_BOOL(r_lighting));
            m_renderer.setDepthPrepassEnabled(CVAR_BOOL(r_depthprepass));
            m_renderer.setBVHEnabled(CVAR_BOOL(r_frustumculling));
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Config: %s", EditorConfig::getConfigPath().string().c_str());
    }
    ImGui::End();
}

void EditorApp::renderSaveAsDialog()
{
    if (!m_show_save_as_dialog) return;

    ImGui::SetNextWindowSize(ImVec2(500.0f, 110.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Save Level As##dialog", &m_show_save_as_dialog,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        ImGui::Text("Save path:");
        ImGui::SetNextItemWidth(-1.0f);
        bool enter = ImGui::InputText("##save_path", m_save_path_buf, sizeof(m_save_path_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::Button("Save", ImVec2(80.0f, 0.0f)) || enter)
        {
            saveLevelAs(std::string(m_save_path_buf));
            m_show_save_as_dialog = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
            m_show_save_as_dialog = false;
    }
    ImGui::End();
}

void EditorApp::renderPackageDialog()
{
    if (!m_show_package_dialog) return;

    ImGui::SetNextWindowSize(ImVec2(520.0f, 330.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Package Project##dialog", &m_show_package_dialog,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        if (m_package_phase == PackagePhase::Configure)
        {
            // --- Configure Phase ---
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(-80.0f);
            ImGui::InputText("##pkg_output", m_package_output_dir, sizeof(m_package_output_dir));
            ImGui::SameLine();
            if (ImGui::Button("Browse..."))
            {
                std::string folder = FileDialog::openFolder("Select Output Directory");
                if (!folder.empty())
                {
                    std::strncpy(m_package_output_dir, folder.c_str(), sizeof(m_package_output_dir) - 1);
                    // Re-run pre-validation after directory change
                    m_package_pre_warnings = ProjectPackager::validateBeforePackage(
                        m_project_manager, PackageConfig{m_package_output_dir, m_package_name,
                            m_package_compile_levels, m_app.getAPIType()});
                }
            }

            ImGui::Text("Package Name:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##pkg_name", m_package_name, sizeof(m_package_name));

            ImGui::Checkbox("Compile levels to binary", &m_package_compile_levels);

            // Show current render API
            const char* api_name = "Unknown";
            switch (m_app.getAPIType())
            {
            case RenderAPIType::D3D11:  api_name = "Direct3D 11"; break;
            case RenderAPIType::D3D12:  api_name = "Direct3D 12"; break;
            case RenderAPIType::Vulkan: api_name = "Vulkan"; break;
            case RenderAPIType::Metal:  api_name = "Metal"; break;
            default: break;
            }
            ImGui::TextDisabled("Shaders: %s", api_name);

            // Pre-validation warnings
            if (!m_package_pre_warnings.empty())
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                for (const auto& warning : m_package_pre_warnings)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "! %s", warning.c_str());
            }

            ImGui::Spacing();

            bool can_package = m_package_output_dir[0] != '\0' && m_package_name[0] != '\0';
            if (!can_package) ImGui::BeginDisabled();

            if (ImGui::Button("Package", ImVec2(100.0f, 0.0f)))
            {
                executePackageProject();
                m_package_phase = PackagePhase::Results;
            }

            if (!can_package) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
                m_show_package_dialog = false;
        }
        else
        {
            // --- Results Phase ---
            if (m_package_result.success)
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Package Complete!");
                ImGui::Spacing();
                ImGui::Text("Files copied: %d", m_package_result.files_copied);
                if (m_package_result.levels_compiled > 0)
                    ImGui::Text("Levels compiled: %d", m_package_result.levels_compiled);

                ImGui::Spacing();
                ImGui::Text("Output:");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                ImGui::TextWrapped("%s", m_package_output_path.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Packaging Failed");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", m_package_result.error_message.c_str());
                ImGui::PopStyleColor();
            }

            // Show warnings if any
            if (!m_package_result.warnings.empty())
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Warnings (%d):", (int)m_package_result.warnings.size());
                ImGui::BeginChild("##pkg_warnings", ImVec2(0, 80), true);
                for (const auto& warning : m_package_result.warnings)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "! %s", warning.c_str());
                ImGui::EndChild();
            }

            ImGui::Spacing();

            // Action buttons
            if (m_package_result.success)
            {
                if (ImGui::Button("Open Output Folder", ImVec2(160.0f, 0.0f)))
                    FileDialog::openFolderInExplorer(m_package_output_path);
                ImGui::SameLine();
            }

            if (ImGui::Button("Package Again", ImVec2(120.0f, 0.0f)))
                m_package_phase = PackagePhase::Configure;

            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(80.0f, 0.0f)))
                m_show_package_dialog = false;
        }
    }
    ImGui::End();
}

void EditorApp::executePackageProject()
{
    PackageConfig config;
    config.output_directory = m_package_output_dir;
    config.package_name = m_package_name;
    config.compile_levels_to_binary = m_package_compile_levels;
    config.target_render_api = m_app.getAPIType();

    m_package_output_path = (std::filesystem::path(config.output_directory) / config.package_name).string();

    m_package_result = ProjectPackager::packageProject(
        m_project_manager, m_level_manager, config);

    // Persist the output directory
    if (auto* cvar = CVAR_PTR(editor_package_output_dir))
        cvar->setString(m_package_output_dir);

    if (!m_package_result.success)
        LOG_ENGINE_ERROR("[Packager] Failed: {}", m_package_result.error_message);
}

void EditorApp::renderGrid()
{
    const int half_extent = 50;
    const float spacing = 1.0f;
    const float y = 0.0f;

    glm::vec3 grid_color(0.35f, 0.35f, 0.35f);
    glm::vec3 axis_x_color(0.8f, 0.2f, 0.2f);
    glm::vec3 axis_z_color(0.2f, 0.2f, 0.8f);

    for (int i = -half_extent; i <= half_extent; i++)
    {
        float pos = static_cast<float>(i) * spacing;

        // Lines along Z axis
        glm::vec3 color_z = (i == 0) ? axis_x_color : grid_color;
        DebugDraw::get().drawLine(
            glm::vec3(pos, y, -half_extent * spacing),
            glm::vec3(pos, y, half_extent * spacing),
            color_z);

        // Lines along X axis
        glm::vec3 color_x = (i == 0) ? axis_z_color : grid_color;
        DebugDraw::get().drawLine(
            glm::vec3(-half_extent * spacing, y, pos),
            glm::vec3(half_extent * spacing, y, pos),
            color_x);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Level operations
// ─────────────────────────────────────────────────────────────────────────────

void EditorApp::newLevel()
{
    m_world.registry.clear();
    m_level_manager.cleanup();
    m_inspector.mesh_path_cache.clear();
    m_hierarchy.selected_entity = entt::null;

    m_level_data = LevelData{};
    m_level_settings.metadata = &m_level_data.metadata;
    m_current_save_path.clear();
    m_state.unsaved_changes = false;
    m_undo.clear();

    applyLightingFromMetadata();
    m_renderer.markBVHDirty();

    LOG_ENGINE_INFO("New level created");
}

void EditorApp::openLevel(const std::string& path)
{
    LevelData new_data;
    if (!m_level_manager.loadLevel(path, new_data))
    {
        LOG_ENGINE_ERROR("Failed to load level: {}", path);
        return;
    }

    m_world.registry.clear();
    m_level_manager.cleanup();
    m_inspector.mesh_path_cache.clear();
    m_hierarchy.selected_entity = entt::null;

    m_level_data = std::move(new_data);
    m_level_settings.metadata = &m_level_data.metadata;

    m_level_manager.instantiateLevel(
        m_level_data, m_world, m_app.getRenderAPI(),
        nullptr, nullptr, nullptr);

    buildMeshPathCache();

    m_current_save_path = path;
    std::strncpy(m_save_path_buf, path.c_str(), sizeof(m_save_path_buf) - 1);
    m_state.unsaved_changes = false;
    m_undo.clear();
    m_undo.pushState(m_level_data, "initial state");

    applyLightingFromMetadata();
    m_renderer.markBVHDirty();

    LOG_ENGINE_INFO("Opened level: {}", path);
}

void EditorApp::saveLevel()
{
    if (m_current_save_path.empty())
    {
        m_show_save_as_dialog = true;
        return;
    }

    LevelData data = buildLevelDataFromECS();
    if (m_level_manager.saveLevelToJSON(m_current_save_path, data))
    {
        m_state.unsaved_changes = false;
        LOG_ENGINE_INFO("Saved level: {}", m_current_save_path);
    }
    else
        LOG_ENGINE_ERROR("Failed to save level: {}", m_current_save_path);
}

void EditorApp::saveLevelAs(const std::string& path)
{
    m_current_save_path = path;
    std::strncpy(m_save_path_buf, path.c_str(), sizeof(m_save_path_buf) - 1);
    saveLevel();
}

void EditorApp::restoreFromSnapshot(const LevelData& snapshot)
{
    // Save selected entity name for best-effort re-selection
    std::string prev_selected_name;
    if (m_hierarchy.selected_entity != entt::null &&
        m_world.registry.valid(m_hierarchy.selected_entity) &&
        m_world.registry.all_of<TagComponent>(m_hierarchy.selected_entity))
    {
        prev_selected_name = m_world.registry.get<TagComponent>(m_hierarchy.selected_entity).name;
    }

    // Clear and restore
    m_world.registry.clear();
    m_level_manager.cleanup();
    m_inspector.mesh_path_cache.clear();
    m_hierarchy.selected_entity = entt::null;

    m_level_data = snapshot;
    m_level_settings.metadata = &m_level_data.metadata;

    m_level_manager.instantiateLevel(
        m_level_data, m_world, m_app.getRenderAPI(),
        nullptr, nullptr, nullptr);

    buildMeshPathCache();
    applyLightingFromMetadata();
    m_renderer.markBVHDirty();

    // Re-select entity by name
    if (!prev_selected_name.empty())
    {
        auto tag_view = m_world.registry.view<TagComponent>();
        for (auto entity : tag_view)
        {
            if (tag_view.get<TagComponent>(entity).name == prev_selected_name)
            {
                m_hierarchy.selected_entity = entity;
                break;
            }
        }
    }

    m_state.unsaved_changes = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization helpers
// ─────────────────────────────────────────────────────────────────────────────

void EditorApp::buildMeshPathCache()
{
    m_inspector.mesh_path_cache.clear();
    auto view = m_world.registry.view<TagComponent>();
    for (auto entity : view)
    {
        const auto& tag = view.get<TagComponent>(entity);
        const LevelEntity* le = findOriginalLevelEntity(tag.name);
        if (le && !le->mesh_path.empty())
            m_inspector.mesh_path_cache[entity] = le->mesh_path;
    }
}

void EditorApp::applyLightingFromMetadata()
{
    m_renderer.set_level_lighting(
        m_level_data.metadata.ambient_light,
        m_level_data.metadata.diffuse_light,
        m_level_data.metadata.light_direction);
}

const LevelEntity* EditorApp::findOriginalLevelEntity(const std::string& name) const
{
    for (const auto& le : m_level_data.entities)
        if (le.name == name) return &le;
    return nullptr;
}

LevelEntity EditorApp::buildLevelEntityFromECS(entt::entity entity) const
{
    const auto& tag = m_world.registry.get<TagComponent>(entity);
    const auto& t   = m_world.registry.get<TransformComponent>(entity);

    LevelEntity le;
    le.name     = tag.name;
    le.position = t.position;
    le.rotation = t.rotation;
    le.scale    = t.scale;

    // Determine entity type from component presence
    bool has_player  = m_world.registry.all_of<PlayerComponent>(entity);
    bool has_freecam = m_world.registry.all_of<FreecamComponent>(entity);
    bool has_rb      = m_world.registry.all_of<RigidBodyComponent>(entity);
    bool has_mesh    = m_world.registry.all_of<MeshComponent>(entity);
    bool has_collider= m_world.registry.all_of<ColliderComponent>(entity);
    bool has_prep    = m_world.registry.all_of<PlayerRepresentationComponent>(entity);
    bool has_pointlight = m_world.registry.all_of<PointLightComponent>(entity);
    bool has_spotlight  = m_world.registry.all_of<SpotLightComponent>(entity);

    if      (has_player)               le.type = EntityType::Player;
    else if (has_freecam)              le.type = EntityType::Freecam;
    else if (has_prep)                 le.type = EntityType::PlayerRep;
    else if (has_pointlight)           le.type = EntityType::PointLight;
    else if (has_spotlight)            le.type = EntityType::SpotLight;
    else if (has_rb && has_collider)   le.type = EntityType::Physical;
    else if (has_collider && !has_mesh)le.type = EntityType::Collidable;
    else if (has_mesh)                 le.type = EntityType::Renderable;
    else                               le.type = EntityType::Static;

    // Mesh path from cache
    auto it = m_inspector.mesh_path_cache.find(entity);
    if (it != m_inspector.mesh_path_cache.end())
        le.mesh_path = it->second;

    // Read live mesh rendering properties (may have been edited in inspector)
    if (has_mesh)
    {
        auto& mc = m_world.registry.get<MeshComponent>(entity);
        if (mc.m_mesh)
        {
            le.culling      = mc.m_mesh->culling;
            le.transparent  = mc.m_mesh->transparent;
            le.visible      = mc.m_mesh->visible;
            le.casts_shadow = mc.m_mesh->casts_shadow;
            le.force_lod    = mc.m_mesh->force_lod;
        }
    }

    // Preserve fields not exposed in inspector from original LevelEntity
    const LevelEntity* orig = findOriginalLevelEntity(tag.name);
    if (orig)
    {
        le.texture_paths      = orig->texture_paths;
        le.collider_mesh_path = orig->collider_mesh_path;
        le.use_mesh_collision = orig->use_mesh_collision;
        le.tracked_player_name = orig->tracked_player_name;
        le.position_offset    = orig->position_offset;
    }

    // RigidBody
    if (has_rb)
    {
        const auto& rb = m_world.registry.get<RigidBodyComponent>(entity);
        le.has_rigidbody = true;
        le.mass          = rb.mass;
        le.apply_gravity = rb.apply_gravity;
    }

    if (has_collider)
        le.has_collider = true;

    // Player component
    if (has_player)
    {
        const auto& pc = m_world.registry.get<PlayerComponent>(entity);
        le.speed             = pc.speed;
        le.jump_force        = pc.jump_force;
        le.mouse_sensitivity = pc.mouse_sensitivity;
    }

    // Freecam component
    if (has_freecam)
    {
        const auto& fc = m_world.registry.get<FreecamComponent>(entity);
        le.movement_speed      = fc.movement_speed;
        le.fast_movement_speed = fc.fast_movement_speed;
        le.mouse_sensitivity   = fc.mouse_sensitivity;
    }

    // Point light component
    if (has_pointlight)
    {
        const auto& pl = m_world.registry.get<PointLightComponent>(entity);
        le.light_color = pl.color;
        le.light_intensity = pl.intensity;
        le.light_range = pl.range;
        le.light_constant_attenuation = pl.constant_attenuation;
        le.light_linear_attenuation = pl.linear_attenuation;
        le.light_quadratic_attenuation = pl.quadratic_attenuation;
    }

    // Spot light component
    if (has_spotlight)
    {
        const auto& sl = m_world.registry.get<SpotLightComponent>(entity);
        le.light_color = sl.color;
        le.light_intensity = sl.intensity;
        le.light_range = sl.range;
        le.light_inner_cone_angle = sl.inner_cone_angle;
        le.light_outer_cone_angle = sl.outer_cone_angle;
        le.light_constant_attenuation = sl.constant_attenuation;
        le.light_linear_attenuation = sl.linear_attenuation;
        le.light_quadratic_attenuation = sl.quadratic_attenuation;
    }

    return le;
}

LevelData EditorApp::buildLevelDataFromECS() const
{
    LevelData out;
    out.metadata = m_level_data.metadata;

    auto view = m_world.registry.view<TagComponent, TransformComponent>();
    for (auto entity : view)
        out.entities.push_back(buildLevelEntityFromECS(entity));

    out.metadata.entity_count = static_cast<int>(out.entities.size());
    return out;
}

// ============================================================================
// Copy / Paste
// ============================================================================

void EditorApp::copySelectedEntity()
{
    if (m_hierarchy.selected_entity == entt::null ||
        !m_world.registry.valid(m_hierarchy.selected_entity))
        return;

    m_entity_clipboard = buildLevelEntityFromECS(m_hierarchy.selected_entity);

    auto it = m_inspector.mesh_path_cache.find(m_hierarchy.selected_entity);
    m_clipboard_mesh_path = (it != m_inspector.mesh_path_cache.end()) ? it->second : "";
}

void EditorApp::pasteEntity()
{
    if (!m_entity_clipboard.has_value())
        return;

    IRenderAPI* api = m_app.getRenderAPI();
    if (!api) return;

    m_undo.pushState(buildLevelDataFromECS(), "paste entity");

    const LevelEntity& le = *m_entity_clipboard;

    auto entity = m_world.registry.create();
    m_world.registry.emplace<TagComponent>(entity, le.name + " (Pasted)");
    m_world.registry.emplace<TransformComponent>(entity, le.position.x, le.position.y, le.position.z);
    auto& t = m_world.registry.get<TransformComponent>(entity);
    t.rotation = le.rotation;
    t.scale    = le.scale;

    // Mesh
    if (!m_clipboard_mesh_path.empty() &&
        (le.type == EntityType::Renderable || le.type == EntityType::Physical || le.type == EntityType::PlayerRep))
    {
        auto& mc = m_world.registry.emplace<MeshComponent>(entity);
        auto mesh_ptr = std::make_shared<mesh>(m_clipboard_mesh_path, api);
        if (mesh_ptr->is_valid)
        {
            mesh_ptr->uploadToGPU(api);
            mesh_ptr->culling      = le.culling;
            mesh_ptr->transparent  = le.transparent;
            mesh_ptr->visible      = le.visible;
            mesh_ptr->casts_shadow = le.casts_shadow;
            mesh_ptr->force_lod    = le.force_lod;
            mc.m_mesh = mesh_ptr;
        }
        m_inspector.mesh_path_cache[entity] = m_clipboard_mesh_path;
    }

    // RigidBody
    if (le.has_rigidbody)
    {
        auto& rb = m_world.registry.emplace<RigidBodyComponent>(entity);
        rb.mass          = le.mass;
        rb.apply_gravity = le.apply_gravity;
    }

    // Collider
    if (le.has_collider)
        m_world.registry.emplace<ColliderComponent>(entity);

    // Player
    if (le.type == EntityType::Player)
    {
        auto& pc = m_world.registry.emplace<PlayerComponent>(entity);
        pc.speed             = le.speed;
        pc.jump_force        = le.jump_force;
        pc.mouse_sensitivity = le.mouse_sensitivity;
    }

    // Freecam
    if (le.type == EntityType::Freecam)
    {
        auto& fc = m_world.registry.emplace<FreecamComponent>(entity);
        fc.movement_speed      = le.movement_speed;
        fc.fast_movement_speed = le.fast_movement_speed;
        fc.mouse_sensitivity   = le.mouse_sensitivity;
    }

    // PlayerRep
    if (le.type == EntityType::PlayerRep)
    {
        auto& pr = m_world.registry.emplace<PlayerRepresentationComponent>(entity);
        pr.position_offset = le.position_offset;
    }

    // Point Light
    if (le.type == EntityType::PointLight)
    {
        auto& pl = m_world.registry.emplace<PointLightComponent>(entity);
        pl.color = le.light_color;
        pl.intensity = le.light_intensity;
        pl.range = le.light_range;
        pl.constant_attenuation = le.light_constant_attenuation;
        pl.linear_attenuation = le.light_linear_attenuation;
        pl.quadratic_attenuation = le.light_quadratic_attenuation;
    }

    // Spot Light
    if (le.type == EntityType::SpotLight)
    {
        auto& sl = m_world.registry.emplace<SpotLightComponent>(entity);
        sl.color = le.light_color;
        sl.intensity = le.light_intensity;
        sl.range = le.light_range;
        sl.inner_cone_angle = le.light_inner_cone_angle;
        sl.outer_cone_angle = le.light_outer_cone_angle;
        sl.constant_attenuation = le.light_constant_attenuation;
        sl.linear_attenuation = le.light_linear_attenuation;
        sl.quadratic_attenuation = le.light_quadratic_attenuation;
    }

    m_hierarchy.selected_entity = entity;
    m_state.unsaved_changes = true;
    m_renderer.markBVHDirty();
}

// ============================================================================
// Project Browser — shown on startup when no --project is given
// ============================================================================

bool EditorApp::runProjectBrowser()
{
    // Release mouse capture so the user can interact with the UI
    SDL_SetRelativeMouseMode(SDL_FALSE);

    // Set window title for the project browser
    SDL_SetWindowTitle(m_app.getWindow(), "Garden Engine");

    // Initialize default paths
    std::strncpy(m_new_project_dir, ".", sizeof(m_new_project_dir) - 1);
    std::strncpy(m_open_project_path, "", sizeof(m_open_project_path) - 1);
    std::strncpy(m_new_project_name, "MyGame", sizeof(m_new_project_name) - 1);

    // Discover available templates
    {
        std::filesystem::path exe_dir = EnginePaths::getExecutableDir();
        std::filesystem::path templates_dir = exe_dir / ".." / "Templates";
        m_available_templates = ProjectManager::discoverTemplates(templates_dir.string());
        m_selected_template = 0;
    }

    bool project_selected = false;

    while (!project_selected)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGuiManager::get().processEvent(&event);
            if (event.type == SDL_QUIT)
                return false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE)
                return false;
        }

        IRenderAPI* render_api = m_app.getRenderAPI();

        // Render an empty scene (sets up the frame properly for the render API)
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.10f, 0.10f, 0.12f));
        render_api->endSceneRender();

        ImGuiManager::get().newFrame();
        RmlUiManager::get().beginFrame();

        ImGuiIO& io = ImGui::GetIO();

        // ---- Fullscreen background window ----
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGuiWindowFlags bg_flags = ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus |
                                    ImGuiWindowFlags_NoScrollbar;

        ImGui::Begin("##ProjectBrowserBG", nullptr, bg_flags);

        // ---- Custom title bar ----
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            float title_h = 48.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1(p0.x + io.DisplaySize.x, p0.y + title_h);

            // Title bar background
            draw->AddRectFilled(p0, p1, IM_COL32(22, 22, 26, 255));

            // Title text
            ImGui::PushFont(nullptr); // default font
            const char* title = "Garden Engine";
            ImVec2 text_size = ImGui::CalcTextSize(title);
            ImVec2 text_pos(p0.x + 20.0f, p0.y + (title_h - text_size.y) * 0.5f);
            draw->AddText(text_pos, IM_COL32(200, 200, 200, 255), title);

            // Subtitle
            const char* subtitle = "Project Browser";
            ImVec2 sub_size = ImGui::CalcTextSize(subtitle);
            ImVec2 sub_pos(p0.x + 20.0f + text_size.x + 16.0f,
                           p0.y + (title_h - sub_size.y) * 0.5f);
            draw->AddText(sub_pos, IM_COL32(120, 120, 130, 255), subtitle);
            ImGui::PopFont();

            // Bottom border line
            draw->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(50, 50, 60, 255));

            ImGui::Dummy(ImVec2(0, title_h + 1));
        }

        // ---- Content area ----
        float panel_w = 560.0f;
        float panel_h = 420.0f;
        float pad_x = (io.DisplaySize.x - panel_w) * 0.5f;
        float pad_y_top = ImGui::GetCursorPosY() + 40.0f;

        ImGui::SetCursorPos(ImVec2(pad_x, pad_y_top));
        ImGui::BeginChild("##ProjectContent", ImVec2(panel_w, panel_h), false);

        // ---- Open Project section ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
        ImGui::TextUnformatted("Open Existing Project");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        float browse_w = 80.0f;
        float open_w = 72.0f;
        float btns_total = browse_w + 4 + open_w + 8;
        ImGui::SetNextItemWidth(panel_w - btns_total);
        ImGui::InputTextWithHint("##open_path", "Path to .garden file...",
                                 m_open_project_path, sizeof(m_open_project_path));
        ImGui::SameLine();
        if (ImGui::Button("Browse##open", ImVec2(browse_w, 0)))
        {
            std::string result = FileDialog::openFile(
                "Open Garden Project",
                "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0");
            if (!result.empty())
                std::strncpy(m_open_project_path, result.c_str(),
                             sizeof(m_open_project_path) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2(open_w, 0)))
        {
            std::string path(m_open_project_path);
            if (!path.empty() && m_project_manager.loadProject(path))
            {
                std::filesystem::current_path(m_project_manager.getProjectRoot());
                LOG_ENGINE_INFO("Opened project '{}'",
                               m_project_manager.getDescriptor().name);
                if (!m_project_manager.getDescriptor().default_level.empty())
                    openLevel(m_project_manager.getDescriptor().default_level);
                project_selected = true;
            }
            else if (!path.empty())
            {
                LOG_ENGINE_ERROR("Failed to open project: {}", path);
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Divider
        {
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(cp.x, cp.y), ImVec2(cp.x + panel_w, cp.y),
                        IM_COL32(50, 50, 60, 255));
            ImGui::Dummy(ImVec2(0, 1));
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // ---- New Project section ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
        ImGui::TextUnformatted("Create New Project");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(panel_w * 0.5f);
        ImGui::InputTextWithHint("Name", "Project name...",
                                 m_new_project_name, sizeof(m_new_project_name));

        // Template selection
        if (!m_available_templates.empty())
        {
            ImGui::SetNextItemWidth(panel_w * 0.5f);
            const char* preview = (m_selected_template >= 0 && m_selected_template < (int)m_available_templates.size())
                ? m_available_templates[m_selected_template].name.c_str()
                : "Select Template...";
            if (ImGui::BeginCombo("Template", preview))
            {
                for (int i = 0; i < (int)m_available_templates.size(); i++)
                {
                    bool is_selected = (m_selected_template == i);
                    if (ImGui::Selectable(m_available_templates[i].name.c_str(), is_selected))
                        m_selected_template = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                "No project templates found. Check your Templates/ directory.");
        }

        ImGui::SetNextItemWidth(panel_w - browse_w - 8);
        ImGui::InputTextWithHint("Directory", "Parent directory...",
                                 m_new_project_dir, sizeof(m_new_project_dir));
        ImGui::SameLine();
        if (ImGui::Button("Browse##dir", ImVec2(browse_w, 0)))
        {
            std::string result = FileDialog::openFolder("Select Project Directory");
            if (!result.empty())
                std::strncpy(m_new_project_dir, result.c_str(),
                             sizeof(m_new_project_dir) - 1);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Create button — accent colored
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.50f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
        bool can_create = !m_available_templates.empty() &&
                          m_selected_template >= 0 &&
                          m_selected_template < (int)m_available_templates.size();
        if (!can_create) ImGui::BeginDisabled();
        if (ImGui::Button("Create Project", ImVec2(160, 36)))
        {
            std::string name(m_new_project_name);
            std::string dir(m_new_project_dir);
            if (!name.empty() && !dir.empty())
            {
                bool success = m_project_manager.createProjectFromTemplate(
                    m_available_templates[m_selected_template].path, dir, name);

                if (success)
                {
                    std::filesystem::current_path(m_project_manager.getProjectRoot());
                    LOG_ENGINE_INFO("Created project '{}' at '{}'",
                                   name, m_project_manager.getProjectRoot());
                    if (!m_project_manager.getDescriptor().default_level.empty())
                        openLevel(m_project_manager.getDescriptor().default_level);
                    project_selected = true;
                }
                else
                {
                    LOG_ENGINE_ERROR("Failed to create project '{}' in '{}'",
                                    name, dir);
                }
            }
        }
        ImGui::PopStyleColor(3);
        if (!can_create) ImGui::EndDisabled();

        ImGui::EndChild(); // ##ProjectContent

        ImGui::End(); // ##ProjectBrowserBG
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

        ImGui::Render();

        // Project browser renders directly to screen (no viewport texture),
        // so use endFrame() which commits the command buffer and presents.
        // renderUI() is for editor mode only (renders to viewport texture).
        render_api->endFrame();
    }

    // Restore window title for the editor
    SDL_SetWindowTitle(m_app.getWindow(), "Garden Level Editor");

    return true;
}
