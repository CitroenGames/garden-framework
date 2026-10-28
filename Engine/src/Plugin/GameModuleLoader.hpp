#pragma once

#include "EngineExport.h"
#include "GameModuleAPI.h"
#include "ECS/ComponentCatalog.hpp"
#include <string>
#include <cstdint>
#include <vector>
#include <array>

// Loads and manages a game module DLL at runtime.
// Handles hot-reload by copying the DLL before loading (Windows locks loaded DLLs).
class ENGINE_API GameModuleLoader
{
public:
    GameModuleLoader();
    // An active loader without an explicit cleanup-confirmed unload is
    // quarantined for process lifetime; destruction never implies cleanup.
    ~GameModuleLoader();

    GameModuleLoader(const GameModuleLoader&) = delete;
    GameModuleLoader& operator=(const GameModuleLoader&) = delete;
    GameModuleLoader(GameModuleLoader&& other) noexcept;
    GameModuleLoader& operator=(GameModuleLoader&& other) = delete;
    // The old image has the same host-cleanup prerequisite as unload().
    [[nodiscard]] bool replaceWith(GameModuleLoader&& other) noexcept;

    // Load a game DLL from an owned generation copy. Source-image loading is
    // deliberately unsupported: a failed copy leaves the loader idle.
    bool load(const std::string& dll_path);

    // Retire the image only after the host has drained all non-catalog
    // callbacks, jobs, reflection and other module-owned resources.
    [[nodiscard]] bool beginRetirement() noexcept;
    [[nodiscard]] bool unload() noexcept;
    // Retry orphaned providers only after the host has drained all non-catalog
    // callbacks, jobs, reflection and other module-owned resources.
    [[nodiscard]] static bool retryRetiredGenerations() noexcept;
    // Fault-injection seam for lifecycle tests; pass nullptr to restore cleanup.
    using CleanupTestHook = void (*)();
    static void setCleanupTestHook(CleanupTestHook hook) noexcept;
    // Permanently retain the image when host-owned callback cleanup was not
    // proven. No automatic retry can safely reconstruct those obligations.
    void quarantine() noexcept;
    bool isComponentProvider() const noexcept { return m_generation && m_is_provider; }

    // Prepare and validate a distinct candidate before releasing the current
    // image. This primitive deliberately does not invoke game lifecycle
    // callbacks: hosts must quiesce callbacks and tear down module-owned
    // resources before requesting a live reload. Hosts with connected server
    // clients must refuse the request before calling this function.
    // Returns false while retaining the active image when candidate validation
    // or generation-copy preparation fails.
    bool reload(const std::string& dll_path);

    // Loads a validated owned candidate without falling back to the source
    // image.  A host uses this while its current session is still playing,
    // then moves the candidate in only after quiescing the old session.
    // The candidate has not registered components or received callbacks.
    bool loadReloadCandidate(const std::string& dll_path);

    bool isLoaded() const { return m_handle != nullptr; }
    // This is the canonical source module path, never the owned generation image.
    const std::string& getLoadedPath() const { return m_source_path; }
    const std::string& getLoadedImagePath() const { return m_loaded_path; }
    const std::string& getOwnedGenerationRoot() const { return m_generation_root; }
    // Valid only while loaded; callers retaining it across unload must copy it.
    const garden::ecs::ComponentOwnerId* getComponentOwnerId() const
    { return m_handle ? &m_component_owner_id : nullptr; }

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
    void* m_generation = nullptr;
    bool m_is_provider = false;
    bool m_retirement_requested = false;
    void* m_handle = nullptr;         // HMODULE or dlopen handle
    std::string m_source_path;        // Original DLL path
    std::string m_loaded_path;        // Active owned generation image
    std::string m_generation_root;    // Private process/session generation root
    std::string m_generation_path;    // Active owned generation directory, if any
    std::vector<std::string> m_owned_generation_files;
    garden::ecs::ComponentOwnerId m_component_owner_id;

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
    bool attachGeneration() noexcept;
    void moveFrom(GameModuleLoader&& other) noexcept;
};
