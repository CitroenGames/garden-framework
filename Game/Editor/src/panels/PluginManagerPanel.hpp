#pragma once

class EditorPluginHost;

// Lists every plugin the host has discovered. Shows load status, last error,
// and offers Load / Unload / Reload buttons per slot. This panel is a
// first-party member of EditorApp, NOT itself a plugin (bootstrap).
class PluginManagerPanel
{
public:
    void bind(EditorPluginHost* host) { m_host = host; }
    void draw(bool* p_open);

private:
    EditorPluginHost* m_host = nullptr;
    int m_selected = -1;
};
