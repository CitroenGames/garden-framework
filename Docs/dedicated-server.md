# Dedicated Server

A dedicated server runs `Server.exe`, which loads your same game DLL **without** any graphics dependencies. To support a server, your DLL must export the optional server hooks defined in `Engine/src/Plugin/GameModuleAPI.h`.

## Required server exports

```cpp
GAME_API bool gardenServerInit(EngineServices* services);
GAME_API void gardenServerShutdown();
GAME_API void gardenServerUpdate(float delta_time);
GAME_API void gardenServerOnLevelLoaded();
GAME_API void gardenServerOnClientConnected(uint16_t client_id);
GAME_API void gardenServerOnClientDisconnected(uint16_t client_id);
```

If any are missing the host treats the DLL as "client-only" and `Server.exe` will refuse to launch with that game module. **All six** must be present for a server build.

`Application*` and `LevelManager*` are still available; `render_api` is a headless `IRenderAPI` (no-op draw calls). Your server code should never depend on rendering — gameplay simulation, physics, networking only.

## Single DLL, two roles

The FPS template ships **one** DLL that exports both client and server entry points. The host (Editor / Game / Server) resolves whichever set of exports it needs and ignores the rest. This keeps shared logic (movement, weapon rules, replication shapes) in one binary.

If you prefer two DLLs, that also works — point `game_module` in the `.garden` file at whichever DLL the host should load. Network PIE in the editor handles this automatically.

## Server lifecycle

```
Server.exe starts
  ├─ gardenGetAPIVersion()           version check
  ├─ gardenGetGameName()             logged
  ├─ gardenRegisterComponents()      same as client — needed for serialisation
  └─ gardenServerInit(services)      open ENet listen socket, build authoritative state

per level
  └─ gardenServerOnLevelLoaded()     spawn server-only entities, init AI

clients connect / disconnect
  ├─ gardenServerOnClientConnected(id)
  └─ gardenServerOnClientDisconnected(id)

per fixed tick
  └─ gardenServerUpdate(dt)          consume client inputs, step physics, broadcast snapshots

shutdown
  └─ gardenServerShutdown()
```

`gardenGameInit` / `gardenGameUpdate` are **not** called on the server — only the server hooks are.

## EngineServices on the server

`services->listen_port` is set by `Server.exe` from `--port` (default 7777). `connect_address` and `connect_port` are unused on the server.

```cpp
static Net::ServerNetworkManager g_server;

GAME_API bool gardenServerInit(EngineServices* services)
{
    g_services = services;
    uint16_t port = services->listen_port ? services->listen_port : 7777;

    g_server.initialize(port, /*max_clients=*/16);
    g_server.setWorld(services->game_world);
    return true;
}
```

## Per-tick simulation

```cpp
GAME_API void gardenServerUpdate(float dt)
{
    g_server.pumpNetworkEvents(dt);                          // pull queued inputs from clients
    uint32_t steps = g_services->game_world->step_physics(dt); // authoritative fixed steps
    g_server.advanceSimulationTicks(steps);                  // network tick follows physics
    runServerGameRules(*g_services->game_world, dt);         // damage, scoring, win conditions
    g_server.publishWorldState();                            // send snapshots on tick cadence
}
```

The world owns fixed-step accumulation. Networking advances from the number of physics steps actually consumed, so catch-up caps, snapshot cadence, and lag-history ticks follow the authoritative simulation instead of render/frame calls. Method names in the FPS template are slightly different - read `Templates/FPSShooter/src/server/ServerModule.cpp` for the canonical version.

## Connection lifecycle

```cpp
GAME_API void gardenServerOnClientConnected(uint16_t client_id)
{
    auto& reg = g_services->game_world->registry;

    // Spawn an entity for the new player
    auto e = PrefabManager::get().spawnAt(reg, "assets/prefabs/player.prefab", 0, 5, 0);
    auto& tag = reg.get<TagComponent>(e);
    tag.name = "Player " + std::to_string(client_id);

    g_server.bindClientToEntity(client_id, e);
}

GAME_API void gardenServerOnClientDisconnected(uint16_t client_id)
{
    g_server.removeClient(client_id);
}
```

## Building a server-only DLL

Same `.buildscript` as the client; just add the server exports to your sources. The host doesn't care which file an export came from. Some teams split into:

```
src/
├── client/
│   └── ClientModule.cpp     # gardenGameInit + gardenGameUpdate
├── server/
│   └── ServerModule.cpp     # gardenServerInit + gardenServerUpdate
└── shared/
    └── ...                  # components, protocols, math
```

Both `*.cpp` files compile into the same DLL. The hosts call whichever exports are relevant to them.

## Running the server

```bat
bin\Server.exe --project C:\dev\MyGame\MyGame.garden --port 7777
```

It runs headless — no window, no rendering — and logs to stdout. Connect a client with `bin\Game.exe --connect <ip> --port 7777`.

## Pitfalls

- **Don't touch `render_api` on the server.** The headless `IRenderAPI` doesn't crash, but any draw you submit is silently dropped — easy to miss when debugging "missing visuals" that aren't supposed to render anyway.
- **Server clocks differ from client clocks.** Don't hardcode tick rates; read the same fixed delta from `world::fixed_delta` on both sides.
- **Reloads.** Hot-reloading the server DLL while clients are connected is messy. For dev, restart the server. For production, design your client to gracefully handle a disconnect-and-reconnect.
