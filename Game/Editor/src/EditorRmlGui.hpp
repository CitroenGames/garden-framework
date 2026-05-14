#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace Rml
{
    class ElementDocument;
    class Event;
    class EventListener;
}

class EditorRmlGui
{
public:
    EditorRmlGui();
    ~EditorRmlGui();

    struct Callbacks
    {
        std::function<void()> on_new_level;
        std::function<void()> on_open_level;
        std::function<void()> on_save_level;
        std::function<void()> on_save_level_as;
        std::function<void()> on_package_project;
        std::function<void()> on_undo;
        std::function<void()> on_redo;
        std::function<void()> on_copy;
        std::function<void()> on_paste;
        std::function<void()> on_duplicate;
        std::function<void()> on_delete;
        std::function<void()> on_settings;
        std::function<void()> on_quit;
        std::function<void()> on_play;
        std::function<void()> on_stop;
        std::function<void()> on_translate;
        std::function<void()> on_rotate;
        std::function<void()> on_scale;
        std::function<void()> on_toggle_snap;
        std::function<void()> on_toggle_grid;
        std::function<void()> on_toggle_viewport;
        std::function<void()> on_toggle_toolbar;
        std::function<void()> on_toggle_hierarchy;
        std::function<void()> on_toggle_inspector;
        std::function<void()> on_toggle_level_settings;
        std::function<void()> on_toggle_content_browser;
        std::function<void()> on_toggle_console;
        std::function<void()> on_toggle_model_preview;
        std::function<void()> on_toggle_status_bar;
        std::function<void()> on_toggle_navmesh;
        std::function<void()> on_toggle_plugin_manager;
        std::function<void()> on_toggle_all_ui;
        std::function<void()> on_toggle_physics_debug;
        std::function<void()> on_toggle_performance;
    };

    struct FrameState
    {
        bool visible = true;
        bool can_edit = true;
        bool can_undo = false;
        bool can_redo = false;
        bool can_copy = false;
        bool can_paste = false;
        bool can_duplicate = false;
        bool can_delete = false;
        bool simulation_active = false;
        bool paused = false;
        bool unsaved_changes = false;
        bool external_pie_active = false;
        bool network_pie_active = false;
        bool show_viewport = true;
        bool show_toolbar = true;
        bool show_hierarchy = true;
        bool show_inspector = true;
        bool show_level_settings = true;
        bool show_content_browser = true;
        bool show_console = true;
        bool show_model_preview = true;
        bool show_status_bar = true;
        bool show_navmesh = false;
        bool show_plugin_manager = false;
        bool show_ui = true;
        bool show_physics_debug = false;
        bool show_performance_monitor = false;
        bool transform_translate = true;
        bool transform_rotate = false;
        bool transform_scale = false;
        bool snap_enabled = false;
        bool grid_enabled = true;
        int spawned_processes = 0;
        float fps = 0.0f;
        bool gpu_ms_valid = false;
        float gpu_ms = 0.0f;
        std::size_t total_entities = 0;
        std::size_t visible_entities = 0;
        std::size_t draw_calls = 0;
        std::string backend_name;
        std::string project_name;
        std::string current_save_path;
    };

    bool initialize(Callbacks callbacks);
    void shutdown();
    void update(const FrameState& state);

    bool isReady() const { return m_document != nullptr; }

private:
    class ActionListener;

    void attachListeners();
    void detachListeners();
    void handleAction(const std::string& id);
    void setActiveMenu(const std::string& menu);
    void closeMenus();
    void syncMenuState();
    void setText(const char* id, const std::string& value);
    void setClass(const char* id, const char* class_name, bool enabled);

    Rml::ElementDocument* m_document = nullptr;
    Callbacks m_callbacks;
    std::unique_ptr<ActionListener> m_listener;
    std::string m_activeMenu;
};
