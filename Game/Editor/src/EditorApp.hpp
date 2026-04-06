#pragma once

#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Graphics/renderer.hpp"
#include "EditorCamera.hpp"
#include "EditorState.hpp"
#include "GameSimulation.hpp"
#include "InputManager.hpp"
#include "Components/camera.hpp"
#include "panels/SceneHierarchyPanel.hpp"
#include "panels/InspectorPanel.hpp"
#include "panels/LevelSettingsPanel.hpp"
#include "panels/ToolbarPanel.hpp"
#include "panels/ConsolePanel.hpp"
#include "panels/ContentBrowserPanel.hpp"
#include "panels/ViewportOverlayPanel.hpp"
#include "panels/StatusBarPanel.hpp"
#include "panels/ViewportPanel.hpp"
#include "panels/NavMeshPanel.hpp"
#include "panels/PhysicsDebugPanel.hpp"
#include "panels/LODSettingsPanel.hpp"
#include "panels/ModelPreviewPanel.hpp"
#include "Assets/AssetScanner.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/ProjectPackager.hpp"
#include "UndoSystem.hpp"
#include <optional>
#include "EditorConfig.hpp"
#include "Plugin/GameModuleLoader.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "PIEProcessManager.hpp"
#include "PIEClientInstance.hpp"

class EditorApp
{
public:
    bool initialize(RenderAPIType api_type);
    void run();
    void shutdown();

    // Set a .garden project file path to load on init (called before initialize)
    void setProjectPath(const std::string& path) { m_project_path = path; }

    // Set the persistent editor config (called before initialize, owned by main)
    void setEditorConfig(EditorConfig* cfg) { m_editor_config = cfg; }

private:
    // Core systems
    Application  m_app;
    world        m_world;
    LevelManager m_level_manager;
    LevelData    m_level_data;   // kept alive for round-trip serialization fidelity
    renderer     m_renderer;
    EditorCamera m_editor_cam;
    bool         m_running = false;

    // Editor state (shared across panels)
    EditorState m_state;

    // UI visibility
    bool m_show_ui             = true;
    bool m_show_hierarchy      = true;
    bool m_show_inspector      = true;
    bool m_show_level_settings = true;
    bool m_show_toolbar        = true;
    bool m_show_console        = true;
    bool m_show_content_browser = true;
    bool m_show_status_bar     = true;
    bool m_show_viewport       = true;
    bool m_show_navmesh_panel  = false;
    bool m_show_physics_debug  = false;
    bool m_show_model_preview  = true;

    // Mouse state (editor camera)
    bool  m_right_mouse = false;
    float m_mouse_dx    = 0.0f;
    float m_mouse_dy    = 0.0f;
    float m_delta_time  = 0.0f;

    // Project path (from --project CLI arg)
    std::string m_project_path;

    // Persistent editor config (owned by main(), lives next to exe)
    EditorConfig* m_editor_config = nullptr;
    bool m_show_editor_settings = false;

    // Save path
    std::string m_current_save_path;

    // Cached window title (to avoid SDL_SetWindowTitle every frame)
    std::string m_last_window_title;

    // File dialog state
    char m_open_path_buf[512];
    char m_save_path_buf[512];
    bool m_show_open_dialog    = false;
    bool m_show_save_as_dialog = false;

    // Panels
    SceneHierarchyPanel  m_hierarchy;
    InspectorPanel       m_inspector;
    LevelSettingsPanel   m_level_settings;
    ToolbarPanel         m_toolbar;
    ConsolePanel         m_console;
    ContentBrowserPanel  m_content_browser;
    ViewportPanel        m_viewport;
    ViewportOverlayPanel m_viewport_overlay;
    StatusBarPanel       m_status_bar;
    NavMeshPanel         m_navmesh_panel;
    PhysicsDebugPanel    m_physics_debug_panel;

    // Asset management
    Assets::AssetScanner m_asset_scanner;
    LODSettingsPanel     m_lod_settings_panel;
    ModelPreviewPanel    m_model_preview;

    // Undo/Redo
    UndoSystem m_undo;

    // Entity clipboard (Ctrl+C / Ctrl+V)
    std::optional<LevelEntity> m_entity_clipboard;
    std::string m_clipboard_mesh_path;

    // --- Play In Editor (PIE) ---
    std::unique_ptr<GameSimulation> m_game_sim;
    std::shared_ptr<InputManager>   m_game_input_manager;

    // Snapshot data (saved on Play, restored on Stop)
    LevelData   m_play_snapshot;
    camera      m_pre_play_editor_cam;
    std::string m_pre_play_selected_name;
    bool        m_mouse_captured_for_game = false; // true when mouse is locked for game input

    // --- Network PIE ---
    GameModuleLoader   m_game_module;              // Player 1 DLL (or only DLL in listen server)
    ReflectionRegistry m_reflection;
    world              m_server_world;             // separate ECS world for listen server
    EngineServices     m_server_services{};
    EngineServices     m_client_services{};
    bool               m_network_pie_active = false; // true when using game DLL for network PIE
    PIEProcessManager  m_pie_processes;

    // Multi-viewport PIE: additional client instances (Player 2-4) for InEditor mode
    std::vector<std::unique_ptr<PIEClientInstance>> m_pie_clients;
    int m_focused_pie_client = -1;                 // -1 = Player 1 (main viewport)

    // PIE state transitions
    void beginPlay();
    void stopPlay();
    void pausePlay();
    void resumePlay();
    void ejectFromPlay();
    void returnToPlay();
    camera& chooseRenderCamera();

    // LOD hot-reload
    void reloadLODsForMesh(const std::string& mesh_path);

    // Per-frame helpers
    void processEvents();
    void renderDockspace();
    void renderMenuBar();
    void renderOpenDialog();
    void renderSaveAsDialog();
    void renderEditorSettings();
    void renderGrid();

    // Project browser (shown before editor when no --project given)
    bool runProjectBrowser();
    void renderProjectBrowser();
    ProjectManager m_project_manager;
    char m_new_project_name[256] = "";
    char m_new_project_dir[512] = "";
    char m_open_project_path[512] = "";
    bool m_show_new_project_popup = false;

    // Template selection
    std::vector<TemplateInfo> m_available_templates;
    int m_selected_template = 0;

    // Packaging
    bool m_show_package_dialog = false;
    bool m_package_compile_levels = false;
    char m_package_output_dir[512] = "";
    char m_package_name[256] = "";

    enum class PackagePhase { Configure, Results };
    PackagePhase m_package_phase = PackagePhase::Configure;
    PackageResult m_package_result;
    std::vector<std::string> m_package_pre_warnings;
    std::string m_package_output_path;

    void renderPackageDialog();
    void executePackageProject();

    // Level operations
    void newLevel();
    void openLevel(const std::string& path);
    void saveLevel();
    void saveLevelAs(const std::string& path);

    // Undo/redo restore
    void restoreFromSnapshot(const LevelData& snapshot);

    // Serialization helpers
    LevelData          buildLevelDataFromECS() const;
    LevelEntity        buildLevelEntityFromECS(entt::entity entity) const;
    const LevelEntity* findOriginalLevelEntity(const std::string& name) const;
    void               buildMeshPathCache();
    void               applyLightingFromMetadata();

    // Copy/paste
    void copySelectedEntity();
    void pasteEntity();
};
