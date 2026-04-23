// Minimal editor plugin — demonstrates the required ABI entry points.
// See Templates/EditorPlugin/README.md for the full integration guide.

#include "Plugin/EditorPluginAPI.h"
#include "Plugin/IEditorPanel.h"
#include "Plugin/EditorPanelRegistry.hpp"
#include "Plugin/MenuRegistry.hpp"
#include "imgui.h"

#include <memory>
#include <string>

namespace {
    EditorServices* g_services = nullptr;

    // Simple sample panel so you can confirm the plugin is loading and
    // registering. Delete or replace in your own plugin.
    class HelloPanel : public IEditorPanel
    {
    public:
        const char* getId()          const override { return "hello_panel"; }
        const char* getDisplayName() const override { return "Hello Plugin"; }
        const char* getDefaultDockSlot() const override { return "Right"; }
        bool        isVisibleByDefault() const override { return true; }

        void draw(bool* p_open) override
        {
            if (!ImGui::Begin("Hello Plugin", p_open)) { ImGui::End(); return; }
            ImGui::TextWrapped(
                "This panel is served by the EditorPlugin template DLL. "
                "It proves that your plugin loaded, registered an IEditorPanel, "
                "and is receiving per-frame draw calls.");

            ImGui::Separator();

            ImGui::TextDisabled("Try toggling this panel via View → Plugins → Hello Plugin");
            ImGui::End();
        }
    };

    void onSayHelloClicked(void* /*user*/)
    {
        if (g_services && g_services->log_info)
            g_services->log_info("Hello from EditorPlugin!");
    }
}

EDITOR_PLUGIN_API int32_t gardenEditorGetAPIVersion()
{
    return GARDEN_EDITOR_PLUGIN_API_VERSION;
}

EDITOR_PLUGIN_API const char* gardenEditorGetPluginName()
{
    return "EditorPlugin";
}

EDITOR_PLUGIN_API const char* gardenEditorGetPluginVersion()
{
    return "0.1.0";
}

EDITOR_PLUGIN_API bool gardenEditorInit(EditorServices* services)
{
    g_services = services;
    if (services && services->log_info)
        services->log_info("EditorPlugin initialized");
    return true;
}

EDITOR_PLUGIN_API void gardenEditorShutdown()
{
    if (g_services && g_services->log_info)
        g_services->log_info("EditorPlugin shutting down");
    g_services = nullptr;
}

EDITOR_PLUGIN_API void gardenEditorRegisterPanels(EditorPanelRegistry* reg, const char* plugin_name)
{
    if (!reg) return;
    reg->add(std::make_unique<HelloPanel>(), plugin_name ? plugin_name : "EditorPlugin");
}

EDITOR_PLUGIN_API void gardenEditorRegisterMenus(MenuRegistry* reg, const char* plugin_name)
{
    if (!reg) return;
    reg->add("Tools/Say Hello", &onSayHelloClicked, nullptr,
             plugin_name ? plugin_name : "EditorPlugin");
}
