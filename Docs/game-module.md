# Game Module

Your game ships as a DLL the host (Editor / Game / Server) loads at runtime. The contract between host and DLL is defined in `Engine/src/Plugin/GameModuleAPI.h`. This page is a practical guide to that contract.

## API version

```cpp
GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}
```

Every export uses C linkage and `GAME_API` (handles `dllexport` / `visibility("default")`):

```cpp
#if defined(_WIN32)
#   define GAME_API extern "C" __declspec(dllexport)
#else
#   define GAME_API extern "C" __attribute__((visibility("default")))
#endif
```

The host checks the version your DLL reports against the version it was built with and refuses to load on mismatch. **Always return the macro, not a literal.**

## Required client exports

```cpp
GAME_API int32_t     gardenGetAPIVersion();
GAME_API const char* gardenGetGameName();
GAME_API bool        gardenGameInit(EngineServices* services);
GAME_API void        gardenGameShutdown();
GAME_API void        gardenRegisterComponents(ReflectionRegistry* registry);
GAME_API void        gardenGameUpdate(float delta_time);
GAME_API void        gardenOnLevelLoaded();
GAME_API void        gardenOnPlayStart();
GAME_API void        gardenOnPlayStop();
```

If any of these is missing, the host refuses to load the DLL. They are listed in call order below.

## EngineServices

The single `EngineServices*` you receive is your handle to everything the host owns. Cache the pointer; don't make copies of the systems it points at.

```cpp
struct EngineServices
{
    world*               game_world;        // ECS registry + physics + camera
    IRenderAPI*          render_api;        // Active backend (D3D12/Vulkan/Metal)
    InputManager*        input_manager;     // Per-frame key/mouse state
    ReflectionRegistry*  reflection;        // Component registration
    Application*         application;       // Host window, time, lifecycle
    LevelManager*        level_manager;     // Load / save / transition levels
    uint32_t             api_version;

    // Network PIE (API 3+) — 0/nullptr means "use defaults"
    const char*          connect_address = nullptr;
    uint16_t             connect_port    = 0;
    uint16_t             listen_port     = 0;
};
```

Common pattern:

```cpp
static EngineServices* g_services = nullptr;

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    // ... set up your subsystems using g_services-> ...
    return true;
}
```

Returning `false` from `gardenGameInit` aborts the load.

## Lifecycle

```
host loads DLL
  ├─ gardenGetAPIVersion()       version check
  ├─ gardenGetGameName()         logged + shown in title bar
  ├─ gardenRegisterComponents()  before any level loads
  └─ gardenGameInit(services)    cache services; build subsystems

per level load
  └─ gardenOnLevelLoaded()       components from JSON already exist on entities

editor only — entering Play-In-Editor
  └─ gardenOnPlayStart()         take a world snapshot, start gameplay timers

per frame
  └─ gardenGameUpdate(dt)        your main tick

editor only — leaving PIE
  └─ gardenOnPlayStop()          tear down per-play state, stop network, etc.

host shuts down
  └─ gardenGameShutdown()        flush state, free any DLL-owned resources
```

### What goes where

- **`gardenGameInit`** — connect to a server, register Rml fonts, allocate persistent state, hook event-bus listeners. Runs once when the DLL is loaded.
- **`gardenRegisterComponents`** — purely registration. No allocations beyond the descriptors. The registry persists across PIE sessions.
- **`gardenOnLevelLoaded`** — runs after entities are deserialised. Good place to find the player entity, attach systems that reference scene data, or apply level-driven settings.
- **`gardenOnPlayStart` / `gardenOnPlayStop`** — only fire in the **editor's** PIE flow. In a standalone `Game.exe`, treat `gardenGameInit` + `gardenOnLevelLoaded` as your "play started". In server flows, mirror the logic in `gardenServerInit`.
- **`gardenGameUpdate`** — your top-level frame tick. Keep it small; dispatch to systems.
- **`gardenGameShutdown`** — symmetric to init. Disconnect networks, flush logs, close files.

## GameMode and GameState

Garden mirrors Unreal's split in ENTT terms: `GameModeBase` exists only on authority worlds, while `GameStateBase` publishes shared state into singleton ECS components such as `GameStateComponent` and `PlayerStateComponent`.

Selection order is URL option (`?game=MyMode`), level override in Level Settings, then `.garden` project defaults (`default_game_mode`, `default_game_state`). Register custom classes in `gardenRegisterComponents` before levels load:

```cpp
#include "GameFramework/GameModeRegistry.hpp"

GAME_API const char* gardenGetGameName() { return "MyGame"; }

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
    (void)registry;

    GameFramework::GameModeRegistry::get().registerGameMode(
        "ArenaGameMode",
        []() { return std::make_unique<ArenaGameMode>(); },
        gardenGetGameName());
}
```

Use the same `source_id` (`gardenGetGameName()` is conventional) for custom `registerGameState` calls. The host unregisters module-owned factories when the DLL unloads.

## Optional server exports

A dedicated server (`bin/Server.exe`) only loads a DLL if these are present:

```cpp
GAME_API bool gardenServerInit(EngineServices* services);
GAME_API void gardenServerShutdown();
GAME_API void gardenServerUpdate(float delta_time);
GAME_API void gardenServerOnLevelLoaded();
GAME_API void gardenServerOnClientConnected(uint16_t client_id);
GAME_API void gardenServerOnClientDisconnected(uint16_t client_id);
```

Missing exports are not an error; the host simply treats them as "this game has no server". If you ship multiplayer, see [Dedicated Server](dedicated-server.md) and the `Templates/FPSShooter/src/server` reference.

## Skeleton DLL

Minimal `GameModule.cpp` that compiles and loads:

```cpp
#include "Plugin/GameModuleAPI.h"
#include "Reflection/ReflectionRegistry.hpp"

static EngineServices* g_services = nullptr;

GAME_API int32_t     gardenGetAPIVersion()                      { return GARDEN_MODULE_API_VERSION; }
GAME_API const char* gardenGetGameName()                        { return "MyGame"; }
GAME_API bool        gardenGameInit(EngineServices* s)          { g_services = s; return true; }
GAME_API void        gardenGameShutdown()                       { g_services = nullptr; }
GAME_API void        gardenRegisterComponents(ReflectionRegistry*) {}
GAME_API void        gardenGameUpdate(float)                    {}
GAME_API void        gardenOnLevelLoaded()                      {}
GAME_API void        gardenOnPlayStart()                        {}
GAME_API void        gardenOnPlayStop()                         {}
```

Drop this into `src/GameModule.cpp`, link `EngineSDK`, and the editor will load it.

## Hot reload

The host **copies your DLL** to a private location before loading, so you can rebuild while the editor is running. To pick up the new binary:

- In the editor: stop PIE if running, then re-enter Play. The new DLL is loaded.
- In `Game.exe`: restart the process.

State held in DLL globals does not survive a reload. Persistent state should live in level JSON, prefabs, archived ConVars, or files you manage yourself.

## Common pitfalls

- **Don't construct your own `world`, `Application`, or `IRenderAPI`.** Use the pointers from `EngineServices`. Constructing your own gives you a parallel ECS no one renders.
- **Don't store raw pointers to entt entities across level loads.** Levels rebuild the registry; entity IDs become invalid. Use stable identifiers (tags, prefab handles) instead.
- **Server hooks are not optional in spirit.** If your design assumes a server, you must export them — the host won't synthesise them for you.
- **`gardenOnLevelLoaded` runs *after* deserialisation.** That's the earliest point your in-level entities exist, so do find/cache logic there, not in `gardenGameInit`.
