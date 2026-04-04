#pragma once

#include "EngineExport.h"
#include "GameModuleAPI.h"
#include <string>
#include <cstdint>

// Loads and manages a game module DLL at runtime.
// Handles hot-reload by copying the DLL before loading (Windows locks loaded DLLs).
class ENGINE_API GameModuleLoader
{
public:
    GameModuleLoader() = default;
    ~GameModuleLoader();

    // Load a game DLL from disk. On Windows, copies to a temp path first.
    bool load(const std::string& dll_path);

    // Unload the currently loaded DLL.
    void unload();

    // Hot-reload: unload current, copy new, load copy.
    // Returns false if the new DLL fails to load (old is already unloaded).
    bool reload(const std::string& dll_path);

    bool isLoaded() const { return m_handle != nullptr; }
    const std::string& getLoadedPath() const { return m_source_path; }

    // ---- Client hooks (required) ----

    int32_t     getAPIVersion();
    const char* getGameName();
    bool        init(EngineServices* services);
    void        shutdown();
    void        registerComponents(ReflectionRegistry* registry);
    void        update(float delta_time);
    void        onLevelLoaded();
    void        onPlayStart();
    void        onPlayStop();

    // ---- Server hooks (optional — resolved at load, nullptr if absent) ----

    bool hasServerSupport() const { return m_fnServerInit != nullptr; }
    bool serverInit(EngineServices* services);
    void serverShutdown();
    void serverUpdate(float delta_time);
    void serverOnLevelLoaded();
    void serverOnClientConnected(uint16_t client_id);
    void serverOnClientDisconnected(uint16_t client_id);

private:
    void* m_handle = nullptr;         // HMODULE or dlopen handle
    std::string m_source_path;        // Original DLL path
    std::string m_loaded_path;        // Temp copy path (for hot-reload)
    int m_reload_counter = 0;         // Increments each reload for unique temp names

    // Client function pointers (required)
    using FnGetAPIVersion       = int32_t(*)();
    using FnGetGameName         = const char*(*)();
    using FnGameInit            = bool(*)(EngineServices*);
    using FnGameShutdown        = void(*)();
    using FnRegisterComponents  = void(*)(ReflectionRegistry*);
    using FnGameUpdate          = void(*)(float);
    using FnOnLevelLoaded       = void(*)();
    using FnOnPlayStart         = void(*)();
    using FnOnPlayStop          = void(*)();

    FnGetAPIVersion      m_fnGetAPIVersion      = nullptr;
    FnGetGameName        m_fnGetGameName         = nullptr;
    FnGameInit           m_fnGameInit            = nullptr;
    FnGameShutdown       m_fnGameShutdown        = nullptr;
    FnRegisterComponents m_fnRegisterComponents  = nullptr;
    FnGameUpdate         m_fnGameUpdate          = nullptr;
    FnOnLevelLoaded      m_fnOnLevelLoaded       = nullptr;
    FnOnPlayStart        m_fnOnPlayStart         = nullptr;
    FnOnPlayStop         m_fnOnPlayStop          = nullptr;

    // Server function pointers (optional)
    using FnServerInit              = bool(*)(EngineServices*);
    using FnServerShutdown          = void(*)();
    using FnServerUpdate            = void(*)(float);
    using FnServerOnLevelLoaded     = void(*)();
    using FnServerOnClientConnected    = void(*)(uint16_t);
    using FnServerOnClientDisconnected = void(*)(uint16_t);

    FnServerInit              m_fnServerInit              = nullptr;
    FnServerShutdown          m_fnServerShutdown          = nullptr;
    FnServerUpdate            m_fnServerUpdate            = nullptr;
    FnServerOnLevelLoaded     m_fnServerOnLevelLoaded     = nullptr;
    FnServerOnClientConnected    m_fnServerOnClientConnected    = nullptr;
    FnServerOnClientDisconnected m_fnServerOnClientDisconnected = nullptr;

    void clearFunctionPointers();
    bool resolveFunctions();
    std::string makeTempPath(const std::string& original);
};
