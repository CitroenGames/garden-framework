#include "EditorPanelRegistry.hpp"
#include "IEditorPanel.h"
#include <cstring>

void EditorPanelRegistry::add(std::unique_ptr<IEditorPanel> panel, const char* plugin_name)
{
    if (!panel) return;
    Entry e;
    e.panel       = std::move(panel);
    // Construct std::string ENTIRELY inside this DLL so we never read a
    // plugin-DLL std::string with a possibly-mismatched layout.
    e.plugin_name = plugin_name ? plugin_name : "";
    e.visible     = e.panel->isVisibleByDefault();
    m_entries.push_back(std::move(e));
}

void EditorPanelRegistry::removeAllFromPlugin(const char* plugin_name)
{
    if (!plugin_name) return;
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->plugin_name == plugin_name)
        {
            if (it->panel) it->panel->onDetach();
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void EditorPanelRegistry::clear()
{
    for (auto& e : m_entries)
        if (e.panel) e.panel->onDetach();
    m_entries.clear();
}

bool* EditorPanelRegistry::findVisibility(const char* panel_id)
{
    if (!panel_id) return nullptr;
    for (auto& e : m_entries)
    {
        if (e.panel && std::strcmp(e.panel->getId(), panel_id) == 0)
            return &e.visible;
    }
    return nullptr;
}
