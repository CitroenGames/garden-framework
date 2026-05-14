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
}

class EditorRmlGui::ActionListener final : public Rml::EventListener
{
public:
    explicit ActionListener(EditorRmlGui& owner) : m_owner(owner) {}

    void ProcessEvent(Rml::Event& event) override
    {
        Rml::Element* element = event.GetTargetElement();
        while (element && element->GetId().empty())
            element = element->GetParentNode();
        if (!element)
            element = event.GetCurrentElement();
        if (!element)
            return;

        m_owner.handleAction(element->GetId());
        event.StopPropagation();
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

    m_callbacks = std::move(callbacks);
    m_listener = std::make_unique<ActionListener>(*this);
    attachListeners();
    return true;
}

void EditorRmlGui::shutdown()
{
    detachListeners();
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
    syncMenuState();

    const std::string project = state.project_name.empty() ? "No project" : state.project_name;
    const std::string path = state.current_save_path.empty() ? "Untitled level" : state.current_save_path;
    const std::string dirty = state.unsaved_changes ? "Modified" : "Saved";
    const std::string mode = state.simulation_active
        ? (state.paused ? "Paused" : (state.external_pie_active ? "External Play" : "Playing"))
        : "Editing";

    setText("project-label", project);
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
        "menu-settings",
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
}

void EditorRmlGui::detachListeners()
{
    if (!m_document || !m_listener)
        return;

    const char* ids[] = {
        "menu-file",
        "menu-edit",
        "menu-view",
        "menu-debug",
        "menu-settings",
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
            element->RemoveEventListener(Rml::EventId::Click, m_listener.get());
    }
}

void EditorRmlGui::handleAction(const std::string& id)
{
    if (id == "menu-file") { setActiveMenu("file"); return; }
    if (id == "menu-edit") { setActiveMenu("edit"); return; }
    if (id == "menu-view") { setActiveMenu("view"); return; }
    if (id == "menu-debug") { setActiveMenu("debug"); return; }
    if (id == "menu-settings") { setActiveMenu("settings"); return; }

    closeMenus();

    if (id == "new-level") callIfSet(m_callbacks.on_new_level);
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
    setClass("menu-settings", "active", m_activeMenu == "settings");
    setClass("file-menu", "open", m_activeMenu == "file");
    setClass("edit-menu", "open", m_activeMenu == "edit");
    setClass("view-menu", "open", m_activeMenu == "view");
    setClass("debug-menu", "open", m_activeMenu == "debug");
    setClass("settings-menu", "open", m_activeMenu == "settings");
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
