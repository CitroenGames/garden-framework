#include "EditorPluginHost.hpp"
#include "EditorPanelRegistry.hpp"
#include "MenuRegistry.hpp"
#include "Assets/AssetManager.hpp"
#include "Utils/Log.hpp"
#include "json.hpp"

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// ---- Platform helpers (mirrors GameModuleLoader.cpp) ----

static void* platformLoad(const std::string& path)
{
#ifdef _WIN32
    return (void*)LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

static void platformUnload(void* handle)
{
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

static void* platformGetProc(void* handle, const char* name)
{
    if (!handle) return nullptr;
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static std::string platformGetError()
{
#ifdef _WIN32
    char buf[512];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(),
                   0, buf, sizeof(buf), nullptr);
    return std::string(buf);
#else
    const char* e = dlerror();
    return e ? std::string(e) : std::string();
#endif
}

// ---- EditorPluginHost ----

EditorPluginHost::~EditorPluginHost()
{
    unloadAll();
}

std::string EditorPluginHost::platformDllExt()
{
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

void EditorPluginHost::setServicesTemplate(const EditorServices& tmpl)
{
    m_services_tmpl  = tmpl;
    m_panel_registry = tmpl.panel_registry;
    m_menu_registry  = tmpl.menu_registry;
}

void EditorPluginHost::setProjectContext(const std::string& project_root,
                                         const std::string& assets_root,
                                         const std::string& plugin_data_dir)
{
    m_project_root_str    = project_root;
    m_assets_root_str     = assets_root;
    m_plugin_data_dir_str = plugin_data_dir;
    m_project.project_root    = m_project_root_str.c_str();
    m_project.assets_root     = m_assets_root_str.c_str();
    m_project.plugin_data_dir = m_plugin_data_dir_str.c_str();
    m_services_tmpl.project = m_project;
}

void EditorPluginHost::setLogSink(EditorLogFn info, EditorLogFn warn, EditorLogFn err)
{
    m_services_tmpl.log_info  = info;
    m_services_tmpl.log_warn  = warn;
    m_services_tmpl.log_error = err;
}

void EditorPluginHost::setBackgroundJobFn(void (*fn)(void*, EditorBackgroundJobFn, const char*))
{
    m_services_tmpl.run_background = fn;
}

// ---- Discovery ----

bool EditorPluginHost::parseManifest(const std::string& path, EditorPluginManifest& out)
{
    try
    {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        nlohmann::json j;
        f >> j;

        if (j.contains("name") && j["name"].is_string())
            out.name = j["name"].get<std::string>();
        if (j.contains("version") && j["version"].is_string())
            out.version = j["version"].get<std::string>();
        if (j.contains("author") && j["author"].is_string())
            out.author = j["author"].get<std::string>();
        if (j.contains("description") && j["description"].is_string())
            out.description = j["description"].get<std::string>();
        if (j.contains("engine_id") && j["engine_id"].is_string())
            out.engine_id = j["engine_id"].get<std::string>();
        if (j.contains("engine_version") && j["engine_version"].is_string())
            out.engine_version = j["engine_version"].get<std::string>();
        if (j.contains("enabled") && j["enabled"].is_boolean())
            out.enabled = j["enabled"].get<bool>();
        if (j.contains("min_editor_api") && j["min_editor_api"].is_number_integer())
            out.min_editor_api = j["min_editor_api"].get<int>();
        if (j.contains("tags") && j["tags"].is_array())
        {
            out.tags.clear();
            for (auto& t : j["tags"])
                if (t.is_string()) out.tags.push_back(t.get<std::string>());
        }

        out.manifest_path = path;
        out.had_manifest  = true;
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ENGINE_WARN("[EditorPluginHost] Failed to parse plugin manifest '{}': {}",
                       path, e.what());
        return false;
    }
}

void EditorPluginHost::rescan()
{
    if (m_scan_dirs.empty())
    {
        LOG_ENGINE_WARN("[EditorPluginHost] rescan() called before any discoverAll() — nothing to do");
        return;
    }

    // Snapshot dirs before unloadAll wipes anything that might rely on them.
    std::vector<std::string> dirs = m_scan_dirs;
    unloadAll();
    m_slots.clear();
    discoverAll(dirs);
    loadAllEnabled();
}

void EditorPluginHost::discoverAll(const std::vector<std::string>& directories)
{
    // Remember for rescan(). Replace rather than append so the second call
    // doesn't accumulate stale paths.
    m_scan_dirs = directories;

    const std::string ext = platformDllExt();

    for (const auto& dir_str : directories)
    {
        std::error_code ec;
        fs::path dir = dir_str;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
            continue;

        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ext) continue;

            // Skip hot-copies so re-scans don't pick them up.
            std::string stem = entry.path().stem().string();
            if (stem.find("_hot_") != std::string::npos) continue;

            EditorPluginSlot slot;
            slot.manifest.file_path = fs::absolute(entry.path(), ec).string();

            // Look for a sidecar plugin.json next to the DLL.
            // Look for a sidecar `.gardenplugin` manifest next to the DLL.
            // GardenCLI's `generate-plugin` deploys this file from the
            // plugin's source folder.
            fs::path manifest_path = entry.path();
            manifest_path.replace_extension(".gardenplugin");
            if (fs::exists(manifest_path, ec))
                parseManifest(manifest_path.string(), slot.manifest);

            if (slot.manifest.name.empty())
                slot.manifest.name = stem;

            if (!slot.manifest.enabled)
                slot.status = EditorPluginStatus::Disabled;

            m_slots.push_back(std::move(slot));
        }
    }

    LOG_ENGINE_INFO("[EditorPluginHost] Discovered {} plugin(s)", m_slots.size());
}

// ---- Loading ----

void EditorPluginHost::loadAllEnabled()
{
    for (size_t i = 0; i < m_slots.size(); ++i)
    {
        if (m_slots[i].manifest.enabled &&
            m_slots[i].status != EditorPluginStatus::Loaded)
        {
            loadSlot(i);
        }
    }
}

bool EditorPluginHost::copyToTemp(EditorPluginSlot& s)
{
    fs::path src(s.manifest.file_path);
    fs::path hot_dir = src.parent_path() / ".hot";
    std::error_code ec;
    fs::create_directories(hot_dir, ec);

    std::string stem = src.stem().string();
    std::string ext  = src.extension().string();
    s.reload_counter++;
    fs::path dst = hot_dir / (stem + "_hot_" + std::to_string(s.reload_counter) + ext);

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        LOG_ENGINE_WARN("[EditorPluginHost] Failed to copy '{}' to temp: {} — loading original",
                       src.string(), ec.message());
        s.loaded_path = s.manifest.file_path;
        return false;
    }
    s.loaded_path = dst.string();

    // Also copy PDB if present (for debuggability of hot-loaded plugins).
    fs::path pdb_src = src; pdb_src.replace_extension(".pdb");
    if (fs::exists(pdb_src, ec))
    {
        fs::path pdb_dst = dst; pdb_dst.replace_extension(".pdb");
        fs::copy_file(pdb_src, pdb_dst, fs::copy_options::overwrite_existing, ec);
    }
    return true;
}

void EditorPluginHost::removeTempCopy(EditorPluginSlot& s)
{
    if (s.loaded_path.empty() || s.loaded_path == s.manifest.file_path) return;
    std::error_code ec;
    fs::remove(s.loaded_path, ec);
    fs::path pdb(s.loaded_path); pdb.replace_extension(".pdb");
    fs::remove(pdb, ec);
    s.loaded_path.clear();
}

bool EditorPluginHost::resolveExports(EditorPluginSlot& s)
{
    if (!s.handle) return false;

    s.fn_get_api       = (EditorPluginSlot::FnGetAPIVersion)    platformGetProc(s.handle, "gardenEditorGetAPIVersion");
    s.fn_get_name      = (EditorPluginSlot::FnGetPluginName)    platformGetProc(s.handle, "gardenEditorGetPluginName");
    s.fn_get_version   = (EditorPluginSlot::FnGetPluginVersion) platformGetProc(s.handle, "gardenEditorGetPluginVersion");
    s.fn_init          = (EditorPluginSlot::FnInit)             platformGetProc(s.handle, "gardenEditorInit");
    s.fn_shutdown      = (EditorPluginSlot::FnShutdown)         platformGetProc(s.handle, "gardenEditorShutdown");
    s.fn_reg_panels    = (EditorPluginSlot::FnRegisterPanels)   platformGetProc(s.handle, "gardenEditorRegisterPanels");
    s.fn_reg_menus     = (EditorPluginSlot::FnRegisterMenus)    platformGetProc(s.handle, "gardenEditorRegisterMenus");
    s.fn_reg_loaders   = (EditorPluginSlot::FnRegisterAssetLoaders)platformGetProc(s.handle, "gardenEditorRegisterAssetLoaders");
    s.fn_on_project    = (EditorPluginSlot::FnOnProjectChanged) platformGetProc(s.handle, "gardenEditorOnProjectChanged");
    s.fn_tick          = (EditorPluginSlot::FnTick)             platformGetProc(s.handle, "gardenEditorTick");

    return s.fn_get_api && s.fn_get_name && s.fn_init && s.fn_shutdown;
}

void EditorPluginHost::clearExports(EditorPluginSlot& s)
{
    s.fn_get_api = nullptr;
    s.fn_get_name = nullptr;
    s.fn_get_version = nullptr;
    s.fn_init = nullptr;
    s.fn_shutdown = nullptr;
    s.fn_reg_panels = nullptr;
    s.fn_reg_menus = nullptr;
    s.fn_reg_loaders = nullptr;
    s.fn_on_project = nullptr;
    s.fn_tick = nullptr;
}

bool EditorPluginHost::loadSlot(size_t index)
{
    if (index >= m_slots.size()) return false;
    EditorPluginSlot& s = m_slots[index];

    if (s.status == EditorPluginStatus::Loaded) return true;

    if (!s.manifest.enabled)
    {
        s.status     = EditorPluginStatus::Disabled;
        s.last_error = "Plugin disabled in manifest";
        return false;
    }

    s.source_path = s.manifest.file_path;
    copyToTemp(s);

    s.handle = platformLoad(s.loaded_path);
    if (!s.handle)
    {
        s.status     = EditorPluginStatus::FailedToLoad;
        s.last_error = "LoadLibrary failed: " + platformGetError();
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
        removeTempCopy(s);
        return false;
    }

    if (!resolveExports(s))
    {
        s.status     = EditorPluginStatus::FailedToLoad;
        s.last_error = "Missing required exports (gardenEditorGetAPIVersion / gardenEditorInit / gardenEditorShutdown)";
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
        platformUnload(s.handle); s.handle = nullptr;
        removeTempCopy(s);
        clearExports(s);
        return false;
    }

    // Version gate — strict equality on the ABI version exported by the DLL.
    int32_t version = s.fn_get_api();
    if (version != GARDEN_EDITOR_PLUGIN_API_VERSION)
    {
        s.status     = EditorPluginStatus::VersionMismatch;
        s.last_error = "API version mismatch: plugin=" + std::to_string(version) +
                       ", editor=" + std::to_string(GARDEN_EDITOR_PLUGIN_API_VERSION);
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
        platformUnload(s.handle); s.handle = nullptr;
        removeTempCopy(s);
        clearExports(s);
        return false;
    }

    // Manifest gate — the .gardenplugin can advertise a minimum editor API
    // it requires. This catches the (foreseeable) case where a plugin was
    // built against a future editor and dropped into an older install.
    if (s.manifest.min_editor_api > GARDEN_EDITOR_PLUGIN_API_VERSION)
    {
        s.status     = EditorPluginStatus::VersionMismatch;
        s.last_error = "Plugin requires editor API >= " +
                       std::to_string(s.manifest.min_editor_api) +
                       " (this editor is at " +
                       std::to_string(GARDEN_EDITOR_PLUGIN_API_VERSION) + ")";
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
        platformUnload(s.handle); s.handle = nullptr;
        removeTempCopy(s);
        clearExports(s);
        return false;
    }

    // Record plugin name from the DLL (overrides filename-derived name).
    if (s.fn_get_name)
    {
        const char* dll_name = s.fn_get_name();
        if (dll_name && *dll_name) s.manifest.name = dll_name;
    }
    if (s.fn_get_version)
    {
        const char* v = s.fn_get_version();
        if (v && *v) s.manifest.version = v;
    }

    // Build services snapshot — api_version filled here.
    EditorServices services = m_services_tmpl;
    services.api_version = GARDEN_EDITOR_PLUGIN_API_VERSION;
    services.project     = m_project;

    // Isolate init failures. C++ exceptions crossing the DLL boundary are
    // undefined — we catch both std::exception and the catch-all.
    bool init_ok = false;
    try
    {
        init_ok = s.fn_init(&services);
    }
    catch (const std::exception& e)
    {
        s.last_error = std::string("Plugin init threw: ") + e.what();
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
    }
    catch (...)
    {
        s.last_error = "Plugin init threw unknown exception";
        LOG_ENGINE_ERROR("[EditorPluginHost] {} — {}", s.manifest.name, s.last_error);
    }

    if (!init_ok)
    {
        if (s.last_error.empty()) s.last_error = "gardenEditorInit returned false";
        s.status = EditorPluginStatus::InitFailed;
        platformUnload(s.handle); s.handle = nullptr;
        removeTempCopy(s);
        clearExports(s);
        return false;
    }

    // Registration phases — each optional, each tagged with plugin name.
    if (s.fn_reg_panels && m_panel_registry)
        s.fn_reg_panels(m_panel_registry, s.manifest.name.c_str());
    if (s.fn_reg_menus && m_menu_registry)
        s.fn_reg_menus(m_menu_registry, s.manifest.name.c_str());
    if (s.fn_reg_loaders && m_services_tmpl.asset_manager)
        s.fn_reg_loaders(m_services_tmpl.asset_manager, s.manifest.name.c_str());

    if (s.fn_on_project)
        s.fn_on_project(&m_project);

    s.status     = EditorPluginStatus::Loaded;
    s.last_error.clear();
    LOG_ENGINE_INFO("[EditorPluginHost] Loaded '{}' v{}",
                   s.manifest.name, s.manifest.version.empty() ? "?" : s.manifest.version);
    return true;
}

void EditorPluginHost::unloadSlot(size_t index)
{
    if (index >= m_slots.size()) return;
    EditorPluginSlot& s = m_slots[index];
    if (!s.handle) return;

    // Evict panels + menus + asset loaders owned by this plugin FIRST, while
    // the plugin's vtables still point into valid code.
    if (m_panel_registry)
        m_panel_registry->removeAllFromPlugin(s.manifest.name.c_str());
    if (m_menu_registry)
        m_menu_registry->removeAllFromPlugin(s.manifest.name.c_str());
    if (m_services_tmpl.asset_manager)
        m_services_tmpl.asset_manager->unregisterLoadersFromSource(s.manifest.name.c_str());

    if (s.fn_shutdown)
    {
        try { s.fn_shutdown(); }
        catch (...) { LOG_ENGINE_WARN("[EditorPluginHost] {} shutdown threw — continuing", s.manifest.name); }
    }

    platformUnload(s.handle);
    s.handle = nullptr;
    clearExports(s);
    removeTempCopy(s);

    s.status = EditorPluginStatus::NotLoaded;
}

bool EditorPluginHost::reloadSlot(size_t index)
{
    if (index >= m_slots.size()) return false;
    LOG_ENGINE_INFO("[EditorPluginHost] Reloading '{}'", m_slots[index].manifest.name);
    unloadSlot(index);
    return loadSlot(index);
}

void EditorPluginHost::unloadAll()
{
    for (size_t i = 0; i < m_slots.size(); ++i)
        unloadSlot(i);
}

void EditorPluginHost::tick(float delta_time)
{
    for (auto& s : m_slots)
    {
        if (s.status != EditorPluginStatus::Loaded || !s.fn_tick) continue;
        try { s.fn_tick(delta_time); }
        catch (...) { /* swallow — plugin tick must not kill editor */ }
    }
}

void EditorPluginHost::onProjectChanged(const std::string& project_root,
                                        const std::string& assets_root,
                                        const std::string& plugin_data_dir)
{
    setProjectContext(project_root, assets_root, plugin_data_dir);
    for (auto& s : m_slots)
    {
        if (s.status != EditorPluginStatus::Loaded || !s.fn_on_project) continue;
        try { s.fn_on_project(&m_project); }
        catch (...) {}
    }
}
