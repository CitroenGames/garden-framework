// QuakeImporter plugin entry points.
//
// Demonstrates every hook an importer plugin typically uses:
//   - gardenEditorRegisterPanels → QuakeImportPanel (tree view + extract)
//   - gardenEditorRegisterMenus  → File/Import/Quake PAK... shortcut
//   - gardenEditorRegisterAssetLoaders → QuakeMdlLoader (AssetManager hook)
//
// The panel pointer is held in plugin-local state so the menu callback can
// toggle it open. On unload, the editor's EditorPanelRegistry evicts the
// panel (see removeAllFromPlugin) and AssetManager evicts the loader (see
// unregisterLoadersFromSource). We don't free the panel ourselves — the
// registry owns it.

#include "Plugin/EditorPluginAPI.h"
#include "Plugin/EditorPanelRegistry.hpp"
#include "Plugin/MenuRegistry.hpp"
#include "Assets/AssetManager.hpp"

#include "QuakeImportPanel.hpp"
#include "QuakeMdlLoader.hpp"

#include <memory>

namespace {
    EditorServices*                 g_services = nullptr;
    QuakeImporter::QuakeImportPanel* g_panel   = nullptr;  // non-owning — registry owns it
}

EDITOR_PLUGIN_API int32_t     gardenEditorGetAPIVersion()   { return GARDEN_EDITOR_PLUGIN_API_VERSION; }
EDITOR_PLUGIN_API const char* gardenEditorGetPluginName()    { return "QuakeImporter"; }
EDITOR_PLUGIN_API const char* gardenEditorGetPluginVersion() { return "0.1.0"; }

EDITOR_PLUGIN_API bool gardenEditorInit(EditorServices* services)
{
    g_services = services;
    if (services && services->log_info)
        services->log_info("[QuakeImporter] initialized");
    return true;
}

EDITOR_PLUGIN_API void gardenEditorShutdown()
{
    g_services = nullptr;
    g_panel    = nullptr;
}

EDITOR_PLUGIN_API void gardenEditorRegisterPanels(EditorPanelRegistry* reg, const char* plugin_name)
{
    if (!reg) return;
    auto panel = std::make_unique<QuakeImporter::QuakeImportPanel>();
    g_panel = panel.get();
    if (g_services) g_panel->onAttach(g_services);
    reg->add(std::move(panel), plugin_name ? plugin_name : "QuakeImporter");
}

static void onImportMenuClicked(void* /*user*/)
{
    // Make the panel visible. The previous approach (g_panel->openDialog())
    // only worked when the panel was already visible, because the editor
    // skips draw() for hidden panels — the deferred-open flag was never
    // read. Setting the registry's visibility flag directly fixes that.
    if (g_services && g_services->panel_registry)
    {
        if (auto* vis = g_services->panel_registry->findVisibility("quake_import_panel"))
            *vis = true;
    }
    if (g_panel) g_panel->openDialog();   // still set the deferred flag for focus
}

EDITOR_PLUGIN_API void gardenEditorRegisterMenus(MenuRegistry* reg, const char* plugin_name)
{
    if (!reg) return;
    reg->add("File/Import/Quake PAK...", &onImportMenuClicked, nullptr,
             plugin_name ? plugin_name : "QuakeImporter");
}

EDITOR_PLUGIN_API void gardenEditorRegisterAssetLoaders(Assets::AssetManager* mgr, const char* /*plugin_name*/)
{
    if (!mgr) return;
    mgr->registerLoader(std::make_unique<QuakeImporter::QuakeMdlLoader>());
}
