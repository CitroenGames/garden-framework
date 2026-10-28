#pragma once

#include "EngineExport.h"
#include "GameModuleAPI.h"
#include <string>
#include <cstdint>
#include <vector>

// Loads and manages a game module DLL at runtime.
// Handles hot-reload by copying the DLL before loading (Windows locks loaded DLLs).
class ENGINE_API GameModuleLoader
{
public:
    GameModuleLoader();
    ~GameModuleLoader();

    // Load a game DLL from disk. The first load uses an owned generation copy
    // when possible, falling back to the source image only when that copy fails.
    bool load(const std::string& dll_path);

    // Unload the currently loaded DLL.
    void unload();

    // Prepare and validate a distinct candidate before releasing the current
    // image. This primitive deliberately does not invoke game lifecycle
    // callbacks: hosts must quiesce callbacks and tear down module-owned
    // resources before requesting a live reload. Hosts with connected server
    // clients must refuse the request before calling this function.
    // Returns false while retaining the active image when candidate validation
    // or generation-copy preparation fails.
    bool reload(const std::string& dll_path);

    bool isLoaded() const { return m_handle != nullptr; }
    // Kept for compatibility: this is the source module path, never a hot copy.
    const std::string& getLoadedPath() const { return m_source_path; }
    const std::string& getLoadedImagePath() const { return m_loaded_path; }
    const std::string& getOwnedGenerationRoot() const { return m_generation_root; }

    // ---- Client hooks (first six are required; lifecycle hooks are optional) ----

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
    void serverPrePhysics(float fixed_delta);
    void serverUpdate(float delta_time);
    void serverOnLevelLoaded();
    void serverOnClientConnected(uint16_t client_id);
    void serverOnClientDisconnected(uint16_t client_id);

private:
    void* m_handle = nullptr;         // HMODULE or dlopen handle
    std::string m_source_path;        // Original DLL path
    std::string m_loaded_path;        // Active source image or owned generation image
    std::string m_generation_root;    // Private process/session generation root
    std::string m_generation_path;    // Active owned generation directory, if any
    std::vector<std::string> m_owned_generation_files;

    // Core client pointers are required; lifecycle pointers are optional.
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
    using FnServerPrePhysics        = void(*)(float);
    using FnServerUpdate            = void(*)(float);
    using FnServerOnLevelLoaded     = void(*)();
    using FnServerOnClientConnected    = void(*)(uint16_t);
    using FnServerOnClientDisconnected = void(*)(uint16_t);

    FnServerInit              m_fnServerInit              = nullptr;
    FnServerShutdown          m_fnServerShutdown          = nullptr;
    FnServerPrePhysics        m_fnServerPrePhysics        = nullptr;
    FnServerUpdate            m_fnServerUpdate            = nullptr;
    FnServerOnLevelLoaded     m_fnServerOnLevelLoaded     = nullptr;
    FnServerOnClientConnected    m_fnServerOnClientConnected    = nullptr;
    FnServerOnClientDisconnected m_fnServerOnClientDisconnected = nullptr;

    void clearFunctionPointers();
    bool resolveFunctions();
};
