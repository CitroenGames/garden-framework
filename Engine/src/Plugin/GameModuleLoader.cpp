#include "GameModuleLoader.hpp"
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// ---- Platform helpers ----

static void* platformLoad(const std::string& path)
{
#ifdef _WIN32
    return (void*)LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW);
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

static const char* platformGetError()
{
#ifdef _WIN32
    static thread_local char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(),
                   0, buf, sizeof(buf), nullptr);
    return buf;
#else
    return dlerror();
#endif
}

// ---- GameModuleLoader ----

GameModuleLoader::~GameModuleLoader()
{
    unload();
}

std::string GameModuleLoader::makeTempPath(const std::string& original)
{
    fs::path p(original);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    fs::path dir = p.parent_path();
    // e.g. MyGame_hot_3.dll
    return (dir / (stem + "_hot_" + std::to_string(m_reload_counter) + ext)).string();
}

bool GameModuleLoader::load(const std::string& dll_path)
{
    if (m_handle)
        unload();

    m_source_path = dll_path;

    // Copy DLL to temp path so the original can be recompiled
    m_loaded_path = makeTempPath(dll_path);
    m_reload_counter++;

    std::error_code ec;
    fs::copy_file(dll_path, m_loaded_path, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        fprintf(stderr, "[GameModule] Failed to copy DLL to temp path: %s\n", ec.message().c_str());
        // Fall back to loading original directly
        m_loaded_path = dll_path;
    }

    // Also copy PDB if it exists (for debugging)
    fs::path pdb_src = fs::path(dll_path).replace_extension(".pdb");
    if (fs::exists(pdb_src))
    {
        fs::path pdb_dst = fs::path(m_loaded_path).replace_extension(".pdb");
        fs::copy_file(pdb_src, pdb_dst, fs::copy_options::overwrite_existing, ec);
    }

    m_handle = platformLoad(m_loaded_path);
    if (!m_handle)
    {
        fprintf(stderr, "[GameModule] Failed to load DLL '%s': %s\n",
                m_loaded_path.c_str(), platformGetError());
        return false;
    }

    if (!resolveFunctions())
    {
        fprintf(stderr, "[GameModule] DLL loaded but missing required exports\n");
        unload();
        return false;
    }

    // Version check
    int32_t version = getAPIVersion();
    if (version != GARDEN_MODULE_API_VERSION)
    {
        fprintf(stderr, "[GameModule] API version mismatch: DLL=%d, Engine=%d\n",
                version, GARDEN_MODULE_API_VERSION);
        unload();
        return false;
    }

    printf("[GameModule] Loaded '%s' (game: %s, server: %s)\n",
           dll_path.c_str(), getGameName(),
           hasServerSupport() ? "yes" : "no");
    return true;
}

void GameModuleLoader::unload()
{
    if (!m_handle) return;

    clearFunctionPointers();
    platformUnload(m_handle);
    m_handle = nullptr;

    // Clean up temp copy
    if (m_loaded_path != m_source_path && !m_loaded_path.empty())
    {
        std::error_code ec;
        fs::remove(m_loaded_path, ec);
        // Also remove temp PDB
        fs::path pdb = fs::path(m_loaded_path).replace_extension(".pdb");
        fs::remove(pdb, ec);
    }
    m_loaded_path.clear();
}

bool GameModuleLoader::reload(const std::string& dll_path)
{
    printf("[GameModule] Hot-reloading '%s'...\n", dll_path.c_str());
    unload();
    return load(dll_path);
}

bool GameModuleLoader::resolveFunctions()
{
    // Required client exports
    m_fnGetAPIVersion     = (FnGetAPIVersion)    platformGetProc(m_handle, "gardenGetAPIVersion");
    m_fnGetGameName       = (FnGetGameName)      platformGetProc(m_handle, "gardenGetGameName");
    m_fnGameInit          = (FnGameInit)         platformGetProc(m_handle, "gardenGameInit");
    m_fnGameShutdown      = (FnGameShutdown)     platformGetProc(m_handle, "gardenGameShutdown");
    m_fnRegisterComponents = (FnRegisterComponents)platformGetProc(m_handle, "gardenRegisterComponents");
    m_fnGameUpdate        = (FnGameUpdate)       platformGetProc(m_handle, "gardenGameUpdate");
    m_fnOnLevelLoaded     = (FnOnLevelLoaded)    platformGetProc(m_handle, "gardenOnLevelLoaded");
    m_fnOnPlayStart       = (FnOnPlayStart)      platformGetProc(m_handle, "gardenOnPlayStart");
    m_fnOnPlayStop        = (FnOnPlayStop)       platformGetProc(m_handle, "gardenOnPlayStop");

    // Optional server exports (nullptr if absent — that's fine)
    m_fnServerInit              = (FnServerInit)             platformGetProc(m_handle, "gardenServerInit");
    m_fnServerShutdown          = (FnServerShutdown)         platformGetProc(m_handle, "gardenServerShutdown");
    m_fnServerUpdate            = (FnServerUpdate)           platformGetProc(m_handle, "gardenServerUpdate");
    m_fnServerOnLevelLoaded     = (FnServerOnLevelLoaded)    platformGetProc(m_handle, "gardenServerOnLevelLoaded");
    m_fnServerOnClientConnected    = (FnServerOnClientConnected)   platformGetProc(m_handle, "gardenServerOnClientConnected");
    m_fnServerOnClientDisconnected = (FnServerOnClientDisconnected)platformGetProc(m_handle, "gardenServerOnClientDisconnected");

    // Only the client exports are required
    return m_fnGetAPIVersion && m_fnGetGameName && m_fnGameInit &&
           m_fnGameShutdown && m_fnRegisterComponents && m_fnGameUpdate;
}

void GameModuleLoader::clearFunctionPointers()
{
    m_fnGetAPIVersion      = nullptr;
    m_fnGetGameName        = nullptr;
    m_fnGameInit           = nullptr;
    m_fnGameShutdown       = nullptr;
    m_fnRegisterComponents = nullptr;
    m_fnGameUpdate         = nullptr;
    m_fnOnLevelLoaded      = nullptr;
    m_fnOnPlayStart        = nullptr;
    m_fnOnPlayStop         = nullptr;

    m_fnServerInit              = nullptr;
    m_fnServerShutdown          = nullptr;
    m_fnServerUpdate            = nullptr;
    m_fnServerOnLevelLoaded     = nullptr;
    m_fnServerOnClientConnected    = nullptr;
    m_fnServerOnClientDisconnected = nullptr;
}

// ---- Client wrappers ----

int32_t GameModuleLoader::getAPIVersion()
{
    return m_fnGetAPIVersion ? m_fnGetAPIVersion() : -1;
}

const char* GameModuleLoader::getGameName()
{
    return m_fnGetGameName ? m_fnGetGameName() : "Unknown";
}

bool GameModuleLoader::init(EngineServices* services)
{
    return m_fnGameInit ? m_fnGameInit(services) : false;
}

void GameModuleLoader::shutdown()
{
    if (m_fnGameShutdown) m_fnGameShutdown();
}

void GameModuleLoader::registerComponents(ReflectionRegistry* registry)
{
    if (m_fnRegisterComponents) m_fnRegisterComponents(registry);
}

void GameModuleLoader::update(float delta_time)
{
    if (m_fnGameUpdate) m_fnGameUpdate(delta_time);
}

void GameModuleLoader::onLevelLoaded()
{
    if (m_fnOnLevelLoaded) m_fnOnLevelLoaded();
}

void GameModuleLoader::onPlayStart()
{
    if (m_fnOnPlayStart) m_fnOnPlayStart();
}

void GameModuleLoader::onPlayStop()
{
    if (m_fnOnPlayStop) m_fnOnPlayStop();
}

// ---- Server wrappers ----

bool GameModuleLoader::serverInit(EngineServices* services)
{
    return m_fnServerInit ? m_fnServerInit(services) : false;
}

void GameModuleLoader::serverShutdown()
{
    if (m_fnServerShutdown) m_fnServerShutdown();
}

void GameModuleLoader::serverUpdate(float delta_time)
{
    if (m_fnServerUpdate) m_fnServerUpdate(delta_time);
}

void GameModuleLoader::serverOnLevelLoaded()
{
    if (m_fnServerOnLevelLoaded) m_fnServerOnLevelLoaded();
}

void GameModuleLoader::serverOnClientConnected(uint16_t client_id)
{
    if (m_fnServerOnClientConnected) m_fnServerOnClientConnected(client_id);
}

void GameModuleLoader::serverOnClientDisconnected(uint16_t client_id)
{
    if (m_fnServerOnClientDisconnected) m_fnServerOnClientDisconnected(client_id);
}
