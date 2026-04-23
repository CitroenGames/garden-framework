#pragma once

#include "EngineExport.h"
#include <string>
#include <vector>

// Allows plugins to insert items into the editor's main menu bar.
// Paths are '/'-delimited, e.g. "File/Import/Quake PAK..." produces nested
// submenus. The editor renders plugin menus AFTER its built-in menus and
// merges identical parent paths across plugins (one "File" menu total).
//
// Callback is plain C function + opaque user pointer to keep the ABI stable
// (no std::function across DLL boundary — crosses allocator boundaries on
// Windows and is a well-known footgun).
class ENGINE_API MenuRegistry
{
public:
    using OnClickFn = void (*)(void* user);

    struct Item
    {
        std::string path;         // "File/Import/Quake PAK..."
        std::string shortcut;     // displayed only; no keybinding dispatch yet
        OnClickFn   on_click = nullptr;
        void*       user     = nullptr;
        std::string plugin_name;
    };

    // ABI note: const char* across the DLL boundary so plugins built in a
    // different configuration than the editor (Release vs Debug) don't crash
    // on std::string layout differences.
    void add(const char* path,
             OnClickFn on_click,
             void* user,
             const char* plugin_name,
             const char* shortcut = nullptr);

    // Remove every menu item contributed by the named plugin.
    void removeAllFromPlugin(const char* plugin_name);
    void clear();

    const std::vector<Item>& items() const { return m_items; }

private:
    std::vector<Item> m_items;
};
