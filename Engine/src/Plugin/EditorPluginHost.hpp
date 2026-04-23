#pragma once

#include "EngineExport.h"
#include "EditorPluginAPI.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class EditorPanelRegistry;
class MenuRegistry;

// Metadata read from a `.gardenplugin` sidecar BEFORE loading the DLL.
// This lets the Plugin Manager panel show entries for disabled plugins.
//
// Schema is defined in `Tools/GardenCLI/src/PluginFile.{hpp,cpp}` and is
// shared with the CLI tool — `garden generate-plugin` deploys a copy of the
// source `.gardenplugin` next to the built DLL, so the editor reads the same
// fields the build tool does.
struct EditorPluginManifest
{
    std::string file_path;        // absolute path to the DLL
    std::string manifest_path;    // absolute path to .gardenplugin (if present)
    std::string name;             // from manifest OR stem of DLL filename
    std::string version;          // from manifest, "" if missing
    std::string author;
    std::string description;
    std::string engine_id;        // optional — lets us flag mismatched-engine builds in UI
    std::string engine_version;
    std::vector<std::string> tags;
    int         min_editor_api = 0;
    bool        enabled        = true;
    bool        had_manifest   = false;
};

// Status of a loaded plugin, shown in the Plugin Manager panel.
enum class EditorPluginStatus : uint8_t
{
    NotLoaded,     // manifest found, DLL not attempted yet
    Loaded,        // init succeeded, plugin is live
    Disabled,      // manifest.enabled == false
    FailedToLoad,  // LoadLibrary failed or required exports missing
    VersionMismatch,
    InitFailed,    // gardenEditorInit returned false or crashed
};

// Represents a single tracked plugin — a manifest + (optionally) a loaded DLL.
// Move-only: the `handle` field is a non-owning OS resource handle that must
// not exist in two slots simultaneously (otherwise unloadAll would FreeLibrary
// the same module twice).
struct EditorPluginSlot
{
    EditorPluginManifest manifest;
    EditorPluginStatus   status       = EditorPluginStatus::NotLoaded;
    std::string          last_error;  // human-readable, surfaced in UI

    // Loaded DLL state (valid iff status == Loaded)
    void*       handle       = nullptr;
    std::string source_path;           // the DLL we were asked to load
    std::string loaded_path;           // temp-copy path actually loaded
    int         reload_counter = 0;

    EditorPluginSlot() = default;
    EditorPluginSlot(EditorPluginSlot&&) noexcept = default;
    EditorPluginSlot& operator=(EditorPluginSlot&&) noexcept = default;
    EditorPluginSlot(const EditorPluginSlot&) = delete;
    EditorPluginSlot& operator=(const EditorPluginSlot&) = delete;

    // Resolved entry points
    using FnGetAPIVersion       = int32_t    (*)();
    using FnGetPluginName       = const char*(*)();
    using FnGetPluginVersion    = const char*(*)();
    using FnInit                = bool       (*)(EditorServices*);
    using FnShutdown            = void       (*)();
    using FnRegisterPanels      = void       (*)(EditorPanelRegistry*, const char*);
    using FnRegisterMenus       = void       (*)(MenuRegistry*, const char*);
    using FnRegisterAssetLoaders= void       (*)(Assets::AssetManager*, const char*);
    using FnOnProjectChanged    = void       (*)(const EditorProjectContext*);
    using FnTick                = void       (*)(float);

    FnGetAPIVersion       fn_get_api         = nullptr;
    FnGetPluginName       fn_get_name        = nullptr;
    FnGetPluginVersion    fn_get_version     = nullptr;
    FnInit                fn_init            = nullptr;
    FnShutdown            fn_shutdown        = nullptr;
    FnRegisterPanels      fn_reg_panels      = nullptr;
    FnRegisterMenus       fn_reg_menus       = nullptr;
    FnRegisterAssetLoaders fn_reg_loaders    = nullptr;
    FnOnProjectChanged    fn_on_project      = nullptr;
    FnTick                fn_tick            = nullptr;
};

// Owns the lifecycle of every editor plugin. Held by EditorApp.
//
// Usage:
//   host.setServicesTemplate(...)  // partial services; host fills api_version
//   host.setRegistries(panel_reg, menu_reg)
//   host.discoverAll({"plugins"})  // reads manifests, does not load DLLs
//   host.loadAllEnabled()          // loads every enabled slot
//   host.tick(dt)                  // per-frame, calls each plugin's fn_tick
//   host.onProjectChanged(ctx)     // broadcasts project change
//   host.unloadAll()               // editor shutdown
class ENGINE_API EditorPluginHost
{
public:
    EditorPluginHost() = default;
    ~EditorPluginHost();
    EditorPluginHost(const EditorPluginHost&) = delete;
    EditorPluginHost& operator=(const EditorPluginHost&) = delete;
    EditorPluginHost(EditorPluginHost&&) = delete;
    EditorPluginHost& operator=(EditorPluginHost&&) = delete;

    // Must be called before loadAllEnabled().
    // The host fills services.api_version and passes a pointer copy to each plugin.
    // NOTE: the `panel_registry`, `menu_registry`, `asset_manager`, `render_api`,
    // and `application` fields are consumed from the template.
    void setServicesTemplate(const EditorServices& tmpl);

    // Sets the project context by COPYING the strings into host-owned storage.
    // EditorProjectContext.* pointers must remain stable for the plugin's
    // lifetime; caller-supplied string lifetimes are not assumed.
    void setProjectContext(const std::string& project_root,
                           const std::string& assets_root,
                           const std::string& plugin_data_dir);

    // Scan directories for plugins. Each directory is searched for *.dll /
    // *.so / *.dylib plus any co-located plugin.json. Duplicate scans
    // replace existing slots (preserving load status when the file is
    // unchanged is a future enhancement).
    void discoverAll(const std::vector<std::string>& directories);

    // Re-run discovery against the directories last passed to discoverAll().
    // Unloads currently-loaded plugins, clears the slot list, scans again,
    // and loads enabled slots. Used by the Plugin Manager's Rescan button so
    // a plugin freshly built (or freshly dropped into the plugins folder)
    // shows up without restarting the editor.
    void rescan();

    // Load every slot whose manifest.enabled == true. Safe to call more than
    // once; already-loaded slots are skipped.
    void loadAllEnabled();

    // Load / unload / reload a specific slot by index. Returns true on
    // success. On failure, slot.status and slot.last_error are updated
    // and false is returned (editor continues running).
    bool loadSlot(size_t index);
    void unloadSlot(size_t index);
    bool reloadSlot(size_t index);

    void unloadAll();

    // Per-frame callback — invokes each loaded plugin's optional fn_tick.
    void tick(float delta_time);

    // Broadcast project change to every loaded plugin. Updates host-owned
    // string storage first so the context pointers stay valid.
    void onProjectChanged(const std::string& project_root,
                          const std::string& assets_root,
                          const std::string& plugin_data_dir);

    // Read-only access for the Plugin Manager UI.
    const std::vector<EditorPluginSlot>& slots() const { return m_slots; }
    std::vector<EditorPluginSlot>&       slots()       { return m_slots; }

    // Set by EditorApp once so plugins registered via host callbacks get
    // routed through the right side-band channels.
    void setLogSink(EditorLogFn info, EditorLogFn warn, EditorLogFn err);
    void setBackgroundJobFn(void (*fn)(void* user, EditorBackgroundJobFn, const char* task_name));

private:
    EditorServices                m_services_tmpl{};   // api_version filled lazily
    EditorProjectContext          m_project{};
    // Stable string storage backing m_project.* const char* pointers.
    std::string                   m_project_root_str;
    std::string                   m_assets_root_str;
    std::string                   m_plugin_data_dir_str;

    EditorPanelRegistry*          m_panel_registry = nullptr;
    MenuRegistry*                 m_menu_registry  = nullptr;
    std::vector<EditorPluginSlot> m_slots;
    // Remembered directories from the last discoverAll(), so rescan() can
    // re-run without the editor having to know how plugin paths were resolved.
    std::vector<std::string>      m_scan_dirs;

    // Helpers
    bool        resolveExports(EditorPluginSlot& s);
    void        clearExports(EditorPluginSlot& s);
    bool        copyToTemp(EditorPluginSlot& s);
    void        removeTempCopy(EditorPluginSlot& s);
    static bool parseManifest(const std::string& path, EditorPluginManifest& out);
    static std::string platformDllExt();
};
