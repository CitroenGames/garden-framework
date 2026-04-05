// Server-side DLL exports for the FPSShooter template.
// These are resolved by the engine's Server.exe launcher via GameModuleLoader.
// They are optional — if absent, the DLL only supports client mode.

#include "Plugin/GameModuleAPI.h"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Components/Components.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/SharedMovement.hpp"
#include "shared/NetworkProtocol.hpp"
#include "shared/NetworkSerializer.hpp"
#include "server/ServerNetworkManager.hpp"
#include "server/GameRules.hpp"
#include "Utils/Log.hpp"

static EngineServices* g_server_services = nullptr;
static Game::ServerNetworkManager g_server_network;
static Game::GameRules g_game_rules;

// ---- Player spawning (called by network callbacks) ----

static void spawnPlayerForClient(uint16_t client_id)
{
    world* w = g_server_services->game_world;

    glm::vec3 spawn_pos = g_game_rules.getNextSpawnPoint();
    entt::entity player_entity = w->registry.create();
    uint32_t network_id = g_server_network.registerEntity(player_entity);

    w->registry.emplace<NetworkedEntity>(player_entity, network_id, client_id, true);

    TransformComponent transform;
    transform.position = spawn_pos;
    w->registry.emplace<TransformComponent>(player_entity, transform);

    RigidBodyComponent rigidbody;
    rigidbody.mass = 1.0f;
    rigidbody.apply_gravity = false;
    w->registry.emplace<RigidBodyComponent>(player_entity, rigidbody);

    PlayerComponent player;
    player.speed = 10.0f;
    player.jump_force = 5.0f;
    player.mouse_sensitivity = 1.0f;
    player.grounded = false;
    player.input_enabled = true;
    w->registry.emplace<PlayerComponent>(player_entity, player);

    g_server_network.setClientPlayerEntity(client_id, network_id);

    // Broadcast spawn to all clients
    SpawnPlayerMessage spawn_msg;
    spawn_msg.client_id = client_id;
    spawn_msg.entity_id = network_id;
    spawn_msg.position = spawn_pos;
    spawn_msg.camera_yaw = 0.0f;

    BitWriter writer;
    NetworkSerializer::serialize(writer, spawn_msg);

    for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
        const ClientInfo* other_client = g_server_network.getClientInfo(i);
        if (other_client && other_client->peer)
            g_server_network.sendReliableToClient(i, writer);
    }

    // Send existing players to the new client
    auto view = w->registry.view<NetworkedEntity, TransformComponent, PlayerComponent>();
    for (auto entity : view) {
        auto& networked = view.get<NetworkedEntity>(entity);
        auto& trans = view.get<TransformComponent>(entity);
        if (networked.owner_client_id == client_id) continue;

        SpawnPlayerMessage existing_msg;
        existing_msg.client_id = networked.owner_client_id;
        existing_msg.entity_id = networked.network_id;
        existing_msg.position = trans.position;
        existing_msg.camera_yaw = 0.0f;

        BitWriter existing_writer;
        NetworkSerializer::serialize(existing_writer, existing_msg);
        g_server_network.sendReliableToClient(client_id, existing_writer);
    }

    LOG_ENGINE_INFO("Spawned player (net_id={}) for client {} at ({},{},{})",
        network_id, client_id, spawn_pos.x, spawn_pos.y, spawn_pos.z);
}

static void despawnPlayerForClient(uint16_t client_id)
{
    world* w = g_server_services->game_world;

    auto view = w->registry.view<NetworkedEntity>();
    for (auto entity : view) {
        auto& networked = view.get<NetworkedEntity>(entity);
        if (networked.owner_client_id == client_id && networked.is_player) {
            DespawnPlayerMessage despawn_msg;
            despawn_msg.client_id = client_id;
            despawn_msg.entity_id = networked.network_id;

            BitWriter writer;
            NetworkSerializer::serialize(writer, despawn_msg);

            for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
                if (i == client_id) continue;
                const ClientInfo* other = g_server_network.getClientInfo(i);
                if (other && other->peer)
                    g_server_network.sendReliableToClient(i, writer);
            }

            g_server_network.unregisterEntity(entity);
            w->registry.destroy(entity);
            LOG_ENGINE_INFO("Despawned player for client {}", client_id);
            break;
        }
    }
}

// ---- Server DLL exports ----

GAME_API bool gardenServerInit(EngineServices* services)
{
    g_server_services = services;

    uint16_t port = services->listen_port ? services->listen_port : 7777;

    if (!g_server_network.initialize() || !g_server_network.startServer(port)) {
        LOG_ENGINE_FATAL("Failed to initialize server network on port {}", port);
        return false;
    }

    g_server_network.setWorld(services->game_world);

    g_server_network.setOnClientConnected([](uint16_t client_id) {
        spawnPlayerForClient(client_id);
    });

    g_server_network.setOnClientDisconnected([](uint16_t client_id) {
        despawnPlayerForClient(client_id);
    });

    LOG_ENGINE_INFO("Server initialized on port {}", port);
    return true;
}

GAME_API void gardenServerShutdown()
{
    g_server_network.shutdown();
    g_server_services = nullptr;
}

GAME_API void gardenServerUpdate(float delta_time)
{
    if (!g_server_services) return;

    world* w = g_server_services->game_world;

    g_server_network.update(delta_time);
    w->step_physics(delta_time);
    g_game_rules.Update(*w, delta_time);
}

GAME_API void gardenServerOnLevelLoaded()
{
    if (!g_server_services) return;
    g_server_services->game_world->getPhysicsSystem().optimizeBroadPhase();
    LOG_ENGINE_INFO("Server level loaded");
}

GAME_API void gardenServerOnClientConnected(uint16_t client_id)
{
    // Already handled via network callback in gardenServerInit
}

GAME_API void gardenServerOnClientDisconnected(uint16_t client_id)
{
    // Already handled via network callback in gardenServerInit
}
