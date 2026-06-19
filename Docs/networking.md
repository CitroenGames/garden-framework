# Networking

Multiplayer in Garden is **client-server with an authoritative server**. Transport is ENet (vendored as ENet6 with IPv6 dual-stack). The full reference implementation is in `Templates/FPSShooter` — copy it before designing from scratch.

## Topology

```
Game.exe (client)  ──────┐
Game.exe (client)  ──────┼──► Server.exe (authoritative)
Game.exe (client)  ──────┘
```

- The **server** is authoritative for world state. It owns the canonical ECS registry, runs physics, processes input, and broadcasts world snapshots.
- **Clients** predict locally for their own player to mask latency, then reconcile when the server's authoritative state arrives.
- Other players are **interpolated** from snapshot history.

The editor can host **Network PIE** — a multi-process play mode where pressing Play spawns one server process and one or more client processes wired together. `EngineServices::connect_address`, `connect_port`, and `listen_port` are passed by the host so your DLL knows whether it's the server or a client.

## Files to know

```
Engine/src/Network/                         # core: ENet wrappers, snapshots, replication
Templates/FPSShooter/src/GameModule.cpp     # client: predict, send input, render others
Templates/FPSShooter/src/server/            # server: authoritative simulation
Templates/FPSShooter/src/shared/            # types shared between client and server
Engine/Thirdparty/enet/                     # ENet6 vendored — IPv6 dual-stack
```

## Client side (in your client DLL)

Cache the network manager:

```cpp
static Net::ClientNetworkManager g_network;

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    g_network.initialize();

    const char* addr = (services->connect_address && *services->connect_address)
                       ? services->connect_address : "127.0.0.1";
    uint16_t port    = services->connect_port ? services->connect_port : 7777;

    g_network.connectToServer(addr, port, "PlayerName");
    g_network.setWorld(services->game_world);
    return true;
}
```

Each frame in `gardenGameUpdate`:

1. Read local input.
2. Pack it into a `MovementInput` / `InputState` and call `g_network.sendInput(...)`.
3. Run client-side prediction for the local player using `Net::SharedMovement` — same code the server runs.
4. Pump network events: `g_network.update(dt)` drains snapshots, applies them, reconciles prediction.

`Net::ReconciliationSmoothing` (in `Network/SharedMovement.hpp`) smooths reconciliation glitches when the server corrects you.

## Server side

A dedicated server is the same DLL with the optional server exports defined. See [Dedicated Server](dedicated-server.md). At minimum:

```cpp
GAME_API bool gardenServerInit(EngineServices* services);
GAME_API void gardenServerUpdate(float dt);
GAME_API void gardenServerOnClientConnected(uint16_t client_id);
GAME_API void gardenServerOnClientDisconnected(uint16_t client_id);
```

The server constructs a `ServerNetworkManager`, listens on `services->listen_port` (or 7777), accepts clients, runs gameplay simulation, and broadcasts snapshots. Authoritative ECS lives on the server's `world*`.

## Replicated components

Snapshots include only the components you mark replicated. The replication layer reads reflected fields, so any custom component you reflect can be replicated by adding it to the replication descriptor (see how `WeaponComponent` and `ScoreComponent` are wired up in `Templates/FPSShooter/src/shared/`).

Delta compression and quantisation are handled by the engine — you don't write the wire format by hand.

## Custom messages

For events that aren't a snapshot field (a shoot result, a chat message, a damage event), use the **custom message handler**:

```cpp
g_network.setCustomMessageHandler([](uint8_t type, Net::BitReader& reader) {
    switch (static_cast<MyMessageType>(type)) {
    case MyMessageType::CHAT: {
        ChatMessage msg;
        if (!MyProtocol::deserialize(reader, msg)) return;
        showChat(msg);
        break;
    }
    /* ... */
    }
});
```

Define your message types and serializers in `src/shared/` so client and server compile from the same code.

## Packet policy

Garden uses three ENet channels:

- `RELIABLE_ORDERED` for connection control, spawns, despawns, cvars, and reliable game events.
- `UNRELIABLE_SEQUENCED` for input commands and world snapshots. Newer state supersedes older state; stale snapshots are dropped on the client.
- `UNRELIABLE_UNORDERED` for fire-and-forget custom messages where ordering does not matter.

The transport layer caps application payloads, drops traffic when a peer's outgoing queue is saturated, and records incoming/outgoing drop counters in `NetworkStats`. If a client misses the acknowledged delta baseline, the server falls back to a full snapshot by default (`net_fullsnapshot_on_baseline_miss 1`).

## ConVars and replication

Some ConVars are flagged `REPLICATED` — the server publishes them to clients. Use this for anything gameplay-sensitive that the server needs to control (gravity overrides, gametime, mod settings). See [Console & ConVars](console-and-convars.md).

## IPv6

The vendored ENet uses `AF_INET6` dual-stack. To probe connectivity on Windows, use the IPv6 stack — testing with IPv4-only `ping` is misleading. Connection strings still accept `127.0.0.1`; the underlying socket maps it.

## Pitfalls

- **Don't run physics on the client and trust it.** Predict locally, but believe the server. The FPSShooter's reconciliation pattern is the right shape.
- **Don't build state into your `world` and assume both sides have it.** Server-only state (loot tables, AI brains) lives in the server module. Client-only state (HUD, particle effects) lives in the client module. The shared truth is the snapshot.
- **One DLL or two?** A single DLL exporting both client *and* server hooks is fine — and what the FPSShooter does. The engine resolves whichever exports the host needs.
- **Hot-reloading network code** can leave clients connected to a stale server. Disconnect from `gardenOnPlayStop` (PIE) or full process restart for safety.
