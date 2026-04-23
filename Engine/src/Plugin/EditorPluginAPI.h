#pragma once

#include <cstdint>

// ============================================================================
// Editor Plugin ABI
//
// Plugins are DLLs loaded by the editor at runtime from ./plugins/. They can:
//   - Register new editor panels (IEditorPanel)
//   - Register new IAssetLoader implementations (AssetManager pipeline)
//   - Register menu items (File/Import/..., Tools/..., etc.)
//   - Receive project lifecycle callbacks
//
// This mirrors GameModuleAPI.h but targets editor extension rather than game
// simulation. One loader idiom, two contracts.
// ============================================================================

#define GARDEN_EDITOR_PLUGIN_API_VERSION 1

// Forward decls — plugin sees these through EngineSDK anyway.
class IRenderAPI;
class Application;
namespace Assets { class AssetManager; class IAssetLoader; }

class IEditorPanel;
class EditorPanelRegistry;
class MenuRegistry;

// Per-project info handed to plugins. All pointers are stable for the
// lifetime of the currently-loaded project.
struct EditorProjectContext
{
    const char* project_root    = nullptr;  // absolute path, e.g. E:/gardenproject/MyGame
    const char* assets_root     = nullptr;  // absolute path, e.g. E:/gardenproject/MyGame/assets
    const char* plugin_data_dir = nullptr;  // <project_root>/.garden/plugin_data/<plugin_name>/
};

// Log routing so plugin output lands in the editor's console panel.
using EditorLogFn = void (*)(const char* msg);

// Background-job submission. Plugin passes an opaque user pointer and a
// function pointer; the editor runs it on a worker thread and guarantees
// (1) the user pointer is valid until the function returns, and (2) the
// function will not run if the plugin is unloaded before it's scheduled.
using EditorBackgroundJobFn = void (*)(void* user);

struct EditorServices
{
    uint32_t              api_version;       // GARDEN_EDITOR_PLUGIN_API_VERSION at load time
    Assets::AssetManager* asset_manager;
    IRenderAPI*           render_api;        // for panels that need GPU textures (e.g. asset thumbnails)
    Application*          application;       // window, input state, event pump
    EditorPanelRegistry*  panel_registry;
    MenuRegistry*         menu_registry;
    EditorProjectContext  project;

    EditorLogFn log_info  = nullptr;
    EditorLogFn log_warn  = nullptr;
    EditorLogFn log_error = nullptr;

    // May be nullptr if the editor is built without a job system (unlikely).
    void (*run_background)(void* user, EditorBackgroundJobFn fn, const char* task_name) = nullptr;
};

// Platform export macro for editor plugin DLLs.
#if defined(_WIN32)
#   define EDITOR_PLUGIN_API extern "C" __declspec(dllexport)
#else
#   define EDITOR_PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

// ============================================================================
// Required exports from every editor plugin DLL:
//
//   EDITOR_PLUGIN_API int32_t     gardenEditorGetAPIVersion();
//   EDITOR_PLUGIN_API const char* gardenEditorGetPluginName();     // "QuakeImporter"
//   EDITOR_PLUGIN_API const char* gardenEditorGetPluginVersion();  // "1.0.0"
//   EDITOR_PLUGIN_API bool        gardenEditorInit(EditorServices* services);
//   EDITOR_PLUGIN_API void        gardenEditorShutdown();
//
// Optional exports (resolved at load, nullptr if absent):
//
//   EDITOR_PLUGIN_API void gardenEditorRegisterPanels(EditorPanelRegistry*, const char* plugin_name);
//   EDITOR_PLUGIN_API void gardenEditorRegisterMenus(MenuRegistry*, const char* plugin_name);
//   EDITOR_PLUGIN_API void gardenEditorRegisterAssetLoaders(Assets::AssetManager*, const char* plugin_name);
//   EDITOR_PLUGIN_API void gardenEditorOnProjectChanged(const EditorProjectContext*);
//   EDITOR_PLUGIN_API void gardenEditorTick(float delta_time);
//
// The `plugin_name` arg is passed back to registries so they can tag owned
// items and evict them cleanly on unload. Plugins should pass it through to
// AssetManager::registerLoader / EditorPanelRegistry::add.
// ============================================================================
