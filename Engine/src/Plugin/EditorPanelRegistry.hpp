#pragma once

#include "EngineExport.h"
#include "IEditorPanel.h"   // unique_ptr<IEditorPanel> needs the complete type
#include <memory>
#include <string>
#include <vector>

// Owns plugin-contributed IEditorPanel instances. One instance per editor.
//
// Thread model: ALL methods must be called from the editor's main thread
// (the thread running EditorApp::run). Plugins should not cache the
// registry pointer for use from background jobs.
class ENGINE_API EditorPanelRegistry
{
public:
    struct Entry
    {
        std::unique_ptr<IEditorPanel> panel;
        std::string plugin_name;  // owner — used for eviction on unload
        bool        visible = false;  // per-panel visibility flag (modified by View menu)

        // Move-only because of unique_ptr; spelled out so the compiler doesn't
        // try to instantiate copy ops via std::vector's implicit operator=.
        Entry() = default;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
    };

    EditorPanelRegistry() = default;
    EditorPanelRegistry(const EditorPanelRegistry&) = delete;
    EditorPanelRegistry& operator=(const EditorPanelRegistry&) = delete;
    EditorPanelRegistry(EditorPanelRegistry&&) noexcept = default;
    EditorPanelRegistry& operator=(EditorPanelRegistry&&) noexcept = default;

    // Takes ownership. `plugin_name` should be the name returned by
    // gardenEditorGetPluginName() — passed through by the editor, not by
    // the plugin directly, so names are consistent.
    //
    // ABI note: const char* (not const std::string&) so this signature is
    // safe to call across DLL boundaries even when the plugin is built in a
    // different configuration than the editor (Release plugin + Debug editor
    // would otherwise crash because their std::string layouts differ).
    void add(std::unique_ptr<IEditorPanel> panel, const char* plugin_name);

    // Remove and destroy all panels contributed by the named plugin.
    // Called by EditorPluginHost during reload/unload.
    void removeAllFromPlugin(const char* plugin_name);

    // Remove every panel (used on editor shutdown).
    void clear();

    std::vector<Entry>&       entries()       { return m_entries; }
    const std::vector<Entry>& entries() const { return m_entries; }

    // Look up a panel's visibility bool by id (for plugin-authored menu items
    // that want to toggle their own panel).
    bool* findVisibility(const char* panel_id);

private:
    std::vector<Entry> m_entries;
};
