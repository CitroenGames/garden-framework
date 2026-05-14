#include "EditorRmlGui.hpp"

#include "UI/RmlUiManager.h"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    fs::path currentPathNoThrow()
    {
        std::error_code error;
        fs::path path = fs::current_path(error);
        if (error)
            return {};
        return path;
    }

    fs::path findEditorShellDocument()
    {
        const fs::path exe_dir = EnginePaths::getExecutableDir();
        const fs::path cwd = currentPathNoThrow();
        const std::vector<fs::path> candidates = {
            exe_dir / ".." / "assets" / "Editor" / "RML" / "EditorShell.rml",
            cwd / "assets" / "Editor" / "RML" / "EditorShell.rml",
            cwd / ".." / "assets" / "Editor" / "RML" / "EditorShell.rml",
        };

        for (const fs::path& path : candidates)
        {
            std::error_code error;
            if (fs::exists(path, error) && !error)
                return path;
        }

        return {};
    }

    fs::path findEditorLogoFile(const fs::path& document_path)
    {
        const fs::path exe_dir = EnginePaths::getExecutableDir();
        const fs::path cwd = currentPathNoThrow();
        const std::vector<fs::path> candidates = {
            document_path.parent_path().parent_path() / "Icons" / "GardenLogo.png",
            cwd / "assets" / "Editor" / "Icons" / "GardenLogo.png",
            cwd / ".." / "assets" / "Editor" / "Icons" / "GardenLogo.png",
            exe_dir / ".." / "assets" / "Editor" / "Icons" / "GardenLogo.png",
        };

        for (const fs::path& path : candidates)
        {
            std::error_code error;
            if (fs::exists(path, error) && !error)
                return path;
        }

        return {};
    }

    std::string escapeRmlText(const std::string& text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char c : text)
        {
            switch (c)
            {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            default: escaped += c; break;
            }
        }
        return escaped;
    }

    std::string roundedNumber(float value)
    {
        return std::to_string(static_cast<int>(std::lround(value)));
    }

    std::string fixedNumber(float value, int precision)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(precision) << value;
        return stream.str();
    }

    void callIfSet(const std::function<void()>& callback)
    {
        if (callback)
            callback();
    }

    void callStringIfSet(const std::function<void(const std::string&)>& callback, const char* value)
    {
        if (callback)
            callback(value);
    }

    void callIntIfSet(const std::function<void(int)>& callback, int value)
    {
        if (callback)
            callback(value);
    }

    std::string menuNameForTriggerId(const std::string& id)
    {
        if (id == "menu-file") return "file";
        if (id == "menu-edit") return "edit";
        if (id == "menu-view") return "view";
        if (id == "menu-debug") return "debug";
        if (id == "toolbar-play-menu") return "play";
        return {};
    }
}

class EditorRmlGui::ActionListener final : public Rml::EventListener
{
public:
    explicit ActionListener(EditorRmlGui& owner) : m_owner(owner) {}

    void ProcessEvent(Rml::Event& event) override
    {
        Rml::Element* element = event.GetCurrentElement();
        if (!element || element->GetId().empty())
        {
            element = event.GetTargetElement();
            while (element && element->GetId().empty())
                element = element->GetParentNode();
        }
        if (!element)
            return;

        m_owner.handleAction(element->GetId());
        event.StopPropagation();
    }

private:
    EditorRmlGui& m_owner;
};

class EditorRmlGui::MenuHoverListener final : public Rml::EventListener
{
public:
    explicit MenuHoverListener(EditorRmlGui& owner) : m_owner(owner) {}

    void ProcessEvent(Rml::Event& event) override
    {
        Rml::Element* element = event.GetCurrentElement();
        if (!element || element->GetId().empty())
        {
            element = event.GetTargetElement();
            while (element && element->GetId().empty())
                element = element->GetParentNode();
        }
        if (!element)
            return;

        m_owner.handleMenuHover(element->GetId());
    }

private:
    EditorRmlGui& m_owner;
};

EditorRmlGui::EditorRmlGui() = default;

EditorRmlGui::~EditorRmlGui()
{
    shutdown();
}

bool EditorRmlGui::initialize(Callbacks callbacks)
{
    shutdown();

    if (!RmlUiManager::get().isInitialized())
        return false;

    const fs::path document_path = findEditorShellDocument();
    if (document_path.empty())
    {
        LOG_ENGINE_WARN("[EditorRmlGui] Failed to find assets/Editor/RML/EditorShell.rml");
        return false;
    }

    m_document = static_cast<Rml::ElementDocument*>(
        RmlUiManager::get().loadEditorDocument(document_path.string().c_str()));
    if (!m_document)
    {
        LOG_ENGINE_WARN("[EditorRmlGui] Failed to load {}", document_path.string());
        return false;
    }

    const fs::path logo_path = findEditorLogoFile(document_path);
    if (!logo_path.empty())
    {
        if (Rml::Element* logo = m_document->GetElementById("engine-logo"))
            logo->SetAttribute("src", logo_path.generic_string());
    }
    else
    {
        LOG_ENGINE_WARN("[EditorRmlGui] Failed to find assets/Editor/Icons/GardenLogo.png");
    }

    m_callbacks = std::move(callbacks);
    m_listener = std::make_unique<ActionListener>(*this);
    m_menuHoverListener = std::make_unique<MenuHoverListener>(*this);
    attachListeners();
    return true;
}

void EditorRmlGui::shutdown()
{
    detachListeners();
    m_menuHoverListener.reset();
    m_listener.reset();

    if (m_document)
    {
        RmlUiManager::get().closeEditorDocument(m_document);
        m_document = nullptr;
    }
}

void EditorRmlGui::update(const FrameState& state)
{
    if (!m_document)
        return;

    setClass("editor-shell", "hidden", !state.visible);
    if (state.simulation_active && m_activeMenu == "play")
        m_activeMenu.clear();
    syncMenuState();

    const std::string project = state.project_name.empty() ? "No project" : state.project_name;
    const std::string path = state.current_save_path.empty() ? "Untitled level" : state.current_save_path;
    const std::string dirty = state.unsaved_changes ? "Modified" : "Saved";
    std::string mode = "Editing";
    if (state.external_pie_active)
        mode = "External Play";
    else if (state.ejected)
        mode = "Ejected";
    else if (state.paused)
        mode = "Paused";
    else if (state.simulation_active)
        mode = "Playing";

    setText("project-label", project);
    setText("project-pill", project);
    setText("top-path", path);
    setText("level-path", path);
    setText("dirty-state", dirty);
    setText("mode-state", mode);
    setText("backend-state", state.backend_name.empty() ? "Renderer unknown" : state.backend_name);
    setText("fps-state", roundedNumber(state.fps) + " FPS");
    setText("gpu-state", state.gpu_ms_valid ? fixedNumber(state.gpu_ms, 2) + " ms GPU" : "GPU --");
    setText("entity-state", std::to_string(state.visible_entities) + "/" + std::to_string(state.total_entities) + " visible");
    setText("draw-state", std::to_string(state.draw_calls) + " draws");
    setText("process-state", state.network_pie_active
        ? std::to_string(state.spawned_processes) + " PIE processes"
        : "Local PIE");
    setText("toolbar-play", state.play_label.empty() ? "Play" : state.play_label);
    setText("play-mode-label", state.launch_mode_label.empty() ? "Selected Viewport" : state.launch_mode_label);
    setText("players-value", std::to_string(state.network_players));
    setText("port-value", std::to_string(state.network_port));
    setText("xr-launch-label", state.xr_launch_label.empty() ? "Meta Quest Link" : state.xr_launch_label);
    setText("xr-runtime", state.xr_runtime_label);
    setText("xr-hmd", state.xr_hmd_label);
    setText("xr-error", state.xr_error);

    setClass("save-level", "disabled", !state.can_edit);
    setClass("save-as-level", "disabled", !state.can_edit);
    setClass("new-level", "disabled", !state.can_edit);
    setClass("open-level", "disabled", !state.can_edit);
    setClass("package-project", "disabled", !state.can_edit);
    setClass("undo", "disabled", !state.can_undo);
    setClass("redo", "disabled", !state.can_redo);
    setClass("copy", "disabled", !state.can_copy);
    setClass("paste", "disabled", !state.can_paste);
    setClass("duplicate", "disabled", !state.can_duplicate);
    setClass("delete", "disabled", !state.can_delete);
    setClass("play", "disabled", state.simulation_active);
    setClass("stop", "disabled", !state.simulation_active);
    setClass("toolbar-save", "disabled", !state.can_edit);
    setClass("toolbar-play", "hidden", state.simulation_active);
    setClass("toolbar-play-menu", "hidden", state.simulation_active);
    setClass("play-mode-label", "hidden", state.simulation_active);
    setClass("toolbar-external", "hidden", !state.external_pie_active);
    setClass("toolbar-pause", "hidden", !state.simulation_active || state.paused || state.ejected || state.external_pie_active);
    setClass("toolbar-resume", "hidden", !state.paused || state.external_pie_active);
    setClass("toolbar-stop", "hidden", !state.simulation_active);
    setClass("toolbar-eject", "hidden", !state.simulation_active || state.paused || state.ejected || state.external_pie_active);
    setClass("toolbar-return", "hidden", !state.ejected || state.external_pie_active);
    setClass("toolbar-stop", "disabled", !state.simulation_active);
    setClass("dirty-state", "modified", state.unsaved_changes);
    setClass("mode-state", "playing", state.simulation_active);
    setClass("statusbar", "hidden", !state.show_status_bar);

    setClass("toggle-viewport", "active", state.show_viewport);
    setClass("toggle-toolbar", "active", state.show_toolbar);
    setClass("toggle-hierarchy", "active", state.show_hierarchy);
    setClass("toggle-inspector", "active", state.show_inspector);
    setClass("toggle-level-settings", "active", state.show_level_settings);
    setClass("toggle-content", "active", state.show_content_browser);
    setClass("toggle-console", "active", state.show_console);
    setClass("toggle-model-preview", "active", state.show_model_preview);
    setClass("toggle-status-bar", "active", state.show_status_bar);
    setClass("toggle-navmesh", "active", state.show_navmesh);
    setClass("toggle-plugin-manager", "active", state.show_plugin_manager);
    setClass("toggle-all-ui", "active", state.show_ui);
    setClass("toggle-physics-debug", "active", state.show_physics_debug);
    setClass("toggle-performance", "active", state.show_performance_monitor);
    setClass("translate", "active", state.transform_translate);
    setClass("rotate", "active", state.transform_rotate);
    setClass("scale", "active", state.transform_scale);
    setClass("snap", "active", state.snap_enabled);
    setClass("grid", "active", state.grid_enabled);
    setClass("launch-selected-viewport", "active", state.launch_selected_viewport);
    setClass("launch-new-window", "active", state.launch_new_editor_window);
    setClass("launch-vr-preview", "active", state.launch_vr_preview);
    setClass("launch-standalone", "active", state.launch_standalone_game);
    setClass("launch-simulate", "active", state.launch_simulate);
    setClass("spawn-current-camera", "active", state.spawn_current_camera);
    setClass("spawn-default-start", "active", state.spawn_default_player_start);
    setClass("net-standalone", "active", state.net_standalone);
    setClass("net-listen", "active", state.net_listen_server);
    setClass("net-dedicated", "active", state.net_dedicated_server);
    setClass("run-in-editor", "active", state.run_in_editor);
    setClass("run-separate", "active", state.run_separate_windows);
    setClass("run-in-editor", "disabled", state.net_standalone);
    setClass("run-separate", "disabled", state.net_standalone);
    setClass("players-row", "disabled", state.net_standalone);
    setClass("players-minus", "disabled", state.net_standalone);
    setClass("players-plus", "disabled", state.net_standalone);
    setClass("port-row", "disabled", state.net_standalone);
    setClass("port-minus", "disabled", state.net_standalone);
    setClass("port-plus", "disabled", state.net_standalone);
    setClass("network-warning", "hidden", state.has_game_module || state.net_standalone);
    setClass("xr-error", "hidden", state.xr_error.empty());
}

void EditorRmlGui::attachListeners()
{
    if (!m_document || !m_listener)
        return;

    const char* ids[] = {
        "menu-file",
        "menu-edit",
        "menu-view",
        "menu-debug",
        "toolbar-save",
        "toolbar-play",
        "toolbar-play-menu",
        "toolbar-pause",
        "toolbar-resume",
        "toolbar-stop",
        "toolbar-eject",
        "toolbar-return",
        "toolbar-settings",
        "new-level",
        "open-level",
        "save-level",
        "save-as-level",
        "package-project",
        "undo",
        "redo",
        "copy",
        "paste",
        "duplicate",
        "delete",
        "settings",
        "quit",
        "play",
        "quick-launch-vr",
        "refresh-openxr",
        "launch-selected-viewport",
        "launch-new-window",
        "launch-vr-preview",
        "launch-standalone",
        "launch-simulate",
        "spawn-current-camera",
        "spawn-default-start",
        "net-standalone",
        "net-listen",
        "net-dedicated",
        "run-in-editor",
        "run-separate",
        "players-minus",
        "players-plus",
        "port-minus",
        "port-plus",
        "stop",
        "translate",
        "rotate",
        "scale",
        "snap",
        "grid",
        "toggle-viewport",
        "toggle-toolbar",
        "toggle-hierarchy",
        "toggle-inspector",
        "toggle-level-settings",
        "toggle-content",
        "toggle-console",
        "toggle-model-preview",
        "toggle-status-bar",
        "toggle-navmesh",
        "toggle-plugin-manager",
        "toggle-all-ui",
        "toggle-physics-debug",
        "toggle-performance",
    };

    for (const char* id : ids)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->AddEventListener(Rml::EventId::Click, m_listener.get());
    }

    const char* hover_ids[] = {
        "menu-file",
        "menu-edit",
        "menu-view",
        "menu-debug",
    };

    for (const char* id : hover_ids)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->AddEventListener(Rml::EventId::Mouseover, m_menuHoverListener.get());
    }
}

void EditorRmlGui::detachListeners()
{
    if (!m_document)
        return;

    const char* ids[] = {
        "menu-file",
        "menu-edit",
        "menu-view",
        "menu-debug",
        "toolbar-save",
        "toolbar-play",
        "toolbar-play-menu",
        "toolbar-pause",
        "toolbar-resume",
        "toolbar-stop",
        "toolbar-eject",
        "toolbar-return",
        "toolbar-settings",
        "new-level",
        "open-level",
        "save-level",
        "save-as-level",
        "package-project",
        "undo",
        "redo",
        "copy",
        "paste",
        "duplicate",
        "delete",
        "settings",
        "quit",
        "play",
        "quick-launch-vr",
        "refresh-openxr",
        "launch-selected-viewport",
        "launch-new-window",
        "launch-vr-preview",
        "launch-standalone",
        "launch-simulate",
        "spawn-current-camera",
        "spawn-default-start",
        "net-standalone",
        "net-listen",
        "net-dedicated",
        "run-in-editor",
        "run-separate",
        "players-minus",
        "players-plus",
        "port-minus",
        "port-plus",
        "stop",
        "translate",
        "rotate",
        "scale",
        "snap",
        "grid",
        "toggle-viewport",
        "toggle-toolbar",
        "toggle-hierarchy",
        "toggle-inspector",
        "toggle-level-settings",
        "toggle-content",
        "toggle-console",
        "toggle-model-preview",
        "toggle-status-bar",
        "toggle-navmesh",
        "toggle-plugin-manager",
        "toggle-all-ui",
        "toggle-physics-debug",
        "toggle-performance",
    };

    for (const char* id : ids)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
        {
            if (m_listener)
                element->RemoveEventListener(Rml::EventId::Click, m_listener.get());
        }
    }

    const char* hover_ids[] = {
        "menu-file",
        "menu-edit",
        "menu-view",
        "menu-debug",
    };

    for (const char* id : hover_ids)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
        {
            if (m_menuHoverListener)
                element->RemoveEventListener(Rml::EventId::Mouseover, m_menuHoverListener.get());
        }
    }
}

void EditorRmlGui::handleAction(const std::string& id)
{
    if (const std::string menu = menuNameForTriggerId(id); !menu.empty())
    {
        setActiveMenu(menu);
        return;
    }

    const bool keep_menu_open = id == "players-minus"
        || id == "players-plus"
        || id == "port-minus"
        || id == "port-plus";
    if (!keep_menu_open)
        closeMenus();

    if (id == "toolbar-save") callIfSet(m_callbacks.on_save_level);
    else if (id == "toolbar-play") callIfSet(m_callbacks.on_play);
    else if (id == "toolbar-pause") callIfSet(m_callbacks.on_pause);
    else if (id == "toolbar-resume") callIfSet(m_callbacks.on_resume);
    else if (id == "toolbar-stop") callIfSet(m_callbacks.on_stop);
    else if (id == "toolbar-eject") callIfSet(m_callbacks.on_eject);
    else if (id == "toolbar-return") callIfSet(m_callbacks.on_return);
    else if (id == "toolbar-settings") callIfSet(m_callbacks.on_settings);
    else if (id == "new-level") callIfSet(m_callbacks.on_new_level);
    else if (id == "open-level") callIfSet(m_callbacks.on_open_level);
    else if (id == "save-level") callIfSet(m_callbacks.on_save_level);
    else if (id == "save-as-level") callIfSet(m_callbacks.on_save_level_as);
    else if (id == "package-project") callIfSet(m_callbacks.on_package_project);
    else if (id == "undo") callIfSet(m_callbacks.on_undo);
    else if (id == "redo") callIfSet(m_callbacks.on_redo);
    else if (id == "copy") callIfSet(m_callbacks.on_copy);
    else if (id == "paste") callIfSet(m_callbacks.on_paste);
    else if (id == "duplicate") callIfSet(m_callbacks.on_duplicate);
    else if (id == "delete") callIfSet(m_callbacks.on_delete);
    else if (id == "settings") callIfSet(m_callbacks.on_settings);
    else if (id == "quit") callIfSet(m_callbacks.on_quit);
    else if (id == "play") callIfSet(m_callbacks.on_play);
    else if (id == "quick-launch-vr") callIfSet(m_callbacks.on_quick_launch_vr);
    else if (id == "refresh-openxr") callIfSet(m_callbacks.on_refresh_openxr);
    else if (id == "launch-selected-viewport") callStringIfSet(m_callbacks.on_set_launch_mode, "selected_viewport");
    else if (id == "launch-new-window") callStringIfSet(m_callbacks.on_set_launch_mode, "new_editor_window");
    else if (id == "launch-vr-preview") callStringIfSet(m_callbacks.on_set_launch_mode, "vr_preview");
    else if (id == "launch-standalone") callStringIfSet(m_callbacks.on_set_launch_mode, "standalone_game");
    else if (id == "launch-simulate") callStringIfSet(m_callbacks.on_set_launch_mode, "simulate");
    else if (id == "spawn-current-camera") callStringIfSet(m_callbacks.on_set_spawn_location, "current_camera");
    else if (id == "spawn-default-start") callStringIfSet(m_callbacks.on_set_spawn_location, "default_player_start");
    else if (id == "net-standalone") callStringIfSet(m_callbacks.on_set_net_mode, "standalone");
    else if (id == "net-listen") callStringIfSet(m_callbacks.on_set_net_mode, "listen_server");
    else if (id == "net-dedicated") callStringIfSet(m_callbacks.on_set_net_mode, "dedicated_server");
    else if (id == "run-in-editor") callStringIfSet(m_callbacks.on_set_run_mode, "in_editor");
    else if (id == "run-separate") callStringIfSet(m_callbacks.on_set_run_mode, "separate_windows");
    else if (id == "players-minus") callIntIfSet(m_callbacks.on_adjust_players, -1);
    else if (id == "players-plus") callIntIfSet(m_callbacks.on_adjust_players, 1);
    else if (id == "port-minus") callIntIfSet(m_callbacks.on_adjust_port, -1);
    else if (id == "port-plus") callIntIfSet(m_callbacks.on_adjust_port, 1);
    else if (id == "stop") callIfSet(m_callbacks.on_stop);
    else if (id == "translate") callIfSet(m_callbacks.on_translate);
    else if (id == "rotate") callIfSet(m_callbacks.on_rotate);
    else if (id == "scale") callIfSet(m_callbacks.on_scale);
    else if (id == "snap") callIfSet(m_callbacks.on_toggle_snap);
    else if (id == "grid") callIfSet(m_callbacks.on_toggle_grid);
    else if (id == "toggle-viewport") callIfSet(m_callbacks.on_toggle_viewport);
    else if (id == "toggle-toolbar") callIfSet(m_callbacks.on_toggle_toolbar);
    else if (id == "toggle-hierarchy") callIfSet(m_callbacks.on_toggle_hierarchy);
    else if (id == "toggle-inspector") callIfSet(m_callbacks.on_toggle_inspector);
    else if (id == "toggle-level-settings") callIfSet(m_callbacks.on_toggle_level_settings);
    else if (id == "toggle-content") callIfSet(m_callbacks.on_toggle_content_browser);
    else if (id == "toggle-console") callIfSet(m_callbacks.on_toggle_console);
    else if (id == "toggle-model-preview") callIfSet(m_callbacks.on_toggle_model_preview);
    else if (id == "toggle-status-bar") callIfSet(m_callbacks.on_toggle_status_bar);
    else if (id == "toggle-navmesh") callIfSet(m_callbacks.on_toggle_navmesh);
    else if (id == "toggle-plugin-manager") callIfSet(m_callbacks.on_toggle_plugin_manager);
    else if (id == "toggle-all-ui") callIfSet(m_callbacks.on_toggle_all_ui);
    else if (id == "toggle-physics-debug") callIfSet(m_callbacks.on_toggle_physics_debug);
    else if (id == "toggle-performance") callIfSet(m_callbacks.on_toggle_performance);
}

void EditorRmlGui::handleMenuHover(const std::string& id)
{
    if (m_activeMenu.empty())
        return;

    const std::string menu = menuNameForTriggerId(id);
    if (menu.empty() || menu == m_activeMenu)
        return;

    m_activeMenu = menu;
    syncMenuState();
}

void EditorRmlGui::setActiveMenu(const std::string& menu)
{
    m_activeMenu = (m_activeMenu == menu) ? std::string() : menu;
    syncMenuState();
}

void EditorRmlGui::closeMenus()
{
    if (m_activeMenu.empty())
        return;

    m_activeMenu.clear();
    syncMenuState();
}

void EditorRmlGui::syncMenuState()
{
    setClass("menu-file", "active", m_activeMenu == "file");
    setClass("menu-edit", "active", m_activeMenu == "edit");
    setClass("menu-view", "active", m_activeMenu == "view");
    setClass("menu-debug", "active", m_activeMenu == "debug");
    setClass("toolbar-play-menu", "active", m_activeMenu == "play");
    setClass("file-menu", "open", m_activeMenu == "file");
    setClass("edit-menu", "open", m_activeMenu == "edit");
    setClass("view-menu", "open", m_activeMenu == "view");
    setClass("debug-menu", "open", m_activeMenu == "debug");
    setClass("play-menu", "open", m_activeMenu == "play");
}

void EditorRmlGui::setText(const char* id, const std::string& value)
{
    if (Rml::Element* element = m_document ? m_document->GetElementById(id) : nullptr)
        element->SetInnerRML(escapeRmlText(value));
}

void EditorRmlGui::setClass(const char* id, const char* class_name, bool enabled)
{
    if (Rml::Element* element = m_document ? m_document->GetElementById(id) : nullptr)
        element->SetClass(class_name, enabled);
}
