#include "MenuRegistry.hpp"

void MenuRegistry::add(const char* path,
                       OnClickFn on_click,
                       void* user,
                       const char* plugin_name,
                       const char* shortcut)
{
    if (!path || !*path || !on_click) return;
    Item it;
    // Construct std::strings entirely inside this DLL — see the ABI note in
    // the header.
    it.path        = path;
    it.on_click    = on_click;
    it.user        = user;
    it.plugin_name = plugin_name ? plugin_name : "";
    it.shortcut    = shortcut    ? shortcut    : "";
    m_items.push_back(std::move(it));
}

void MenuRegistry::removeAllFromPlugin(const char* plugin_name)
{
    if (!plugin_name) return;
    for (auto it = m_items.begin(); it != m_items.end();)
    {
        if (it->plugin_name == plugin_name)
            it = m_items.erase(it);
        else
            ++it;
    }
}

void MenuRegistry::clear()
{
    m_items.clear();
}
