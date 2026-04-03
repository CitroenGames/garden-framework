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

class EditorApp
{
public:
    bool initialize(RenderAPIType api_type);
    void run();
    void shutdown();

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

    // Mouse state (editor camera)
    bool  m_right_mouse = false;
    float m_mouse_dx    = 0.0f;
    float m_mouse_dy    = 0.0f;
    float m_delta_time  = 0.0f;

    // Save path
    std::string m_current_save_path;

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

    // --- Play In Editor (PIE) ---
    std::unique_ptr<GameSimulation> m_game_sim;
    std::shared_ptr<InputManager>   m_game_input_manager;

    // Snapshot data (saved on Play, restored on Stop)
    LevelData   m_play_snapshot;
    camera      m_pre_play_editor_cam;
    std::string m_pre_play_selected_name;
    bool        m_mouse_captured_for_game = false; // true when mouse is locked for game input

    // PIE state transitions
    void beginPlay();
    void stopPlay();
    void pausePlay();
    void resumePlay();
    void ejectFromPlay();
    void returnToPlay();
    camera& chooseRenderCamera();

    // Per-frame helpers
    void processEvents();
    void renderDockspace();
    void renderMenuBar();
    void renderOpenDialog();
    void renderSaveAsDialog();
    void renderGrid();

    // Level operations
    void newLevel();
    void openLevel(const std::string& path);
    void saveLevel();
    void saveLevelAs(const std::string& path);

    // Serialization helpers
    LevelData          buildLevelDataFromECS() const;
    const LevelEntity* findOriginalLevelEntity(const std::string& name) const;
    void               buildMeshPathCache();
    void               applyLightingFromMetadata();
};
