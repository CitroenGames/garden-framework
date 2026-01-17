#include <stdio.h>
#include <stdlib.h>

#define SDL_MAIN_HANDLED
#define ENET_IMPLEMENTATION
#include "enet.h"

#include "Utils/CrashHandler.hpp"
#include "Utils/Log.hpp"
#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "SharedComponents.hpp"
#include "GameRules.hpp"
#include "ServerNetworkManager.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static Application app;
static world _world;
static Game::ServerNetworkManager _network;

static void shutdown_server(int code)
{
    _network.shutdown();
    app.shutdown();
    EE::CLog::Shutdown();
    exit(code);
}

int main(int argc, char* argv[])
{
    Paingine2D::CrashHandler* crashHandler = Paingine2D::CrashHandler::GetInstance();
    crashHandler->Initialize("Server");
    EE::CLog::Init();

    LOG_ENGINE_INFO("Starting Headless Server...");

    // Initialize game rules
    Game::GameRules game_rules;

    // Initialize Network
    if (!_network.initialize() || !_network.startServer(7777)) {
        LOG_ENGINE_FATAL("Failed to initialize Server Network");
        return 1;
    }

    // Set up network callbacks
    _network.setOnClientConnected([&](uint16_t client_id) {
        LOG_ENGINE_INFO("Client {0} connected, spawning player", client_id);

        // Get spawn point from game rules
        vector3f spawn_pos = game_rules.getNextSpawnPoint();

        // Create player entity
        entt::entity player_entity = _world.registry.create();

        // Register entity with network manager and get network ID
        uint32_t network_id = _network.registerEntity(player_entity);

        // Add networked entity component
        _world.registry.emplace<NetworkedEntity>(player_entity, network_id, client_id, true);

        // Add transform component
        TransformComponent transform;
        transform.position = spawn_pos;
        _world.registry.emplace<TransformComponent>(player_entity, transform);

        // Add rigidbody component
        RigidBodyComponent rigidbody;
        rigidbody.mass = 1.0f;
        rigidbody.apply_gravity = true;
        _world.registry.emplace<RigidBodyComponent>(player_entity, rigidbody);

        // Add player component
        PlayerComponent player;
        player.speed = 10.0f;
        player.jump_force = 5.0f;
        player.mouse_sensitivity = 1.0f;
        player.grounded = false;
        player.input_enabled = true;
        _world.registry.emplace<PlayerComponent>(player_entity, player);

        // Update client info with player entity network ID
        _network.setClientPlayerEntity(client_id, network_id);

        // Send SPAWN_PLAYER message to all clients (so they see the new player)
        SpawnPlayerMessage spawn_msg;
        spawn_msg.client_id = client_id;
        spawn_msg.entity_id = network_id;
        spawn_msg.position = spawn_pos;
        spawn_msg.camera_yaw = 0.0f;

        BitWriter writer;
        NetworkSerializer::serialize(writer, spawn_msg);

        // Broadcast to all connected clients
        for (uint16_t i = 1; i < _network.getNextClientId(); i++) {
            const ClientInfo* other_client = _network.getClientInfo(i);
            if (other_client && other_client->peer) {
                _network.sendReliableToClient(i, writer);
            }
        }

        // Send existing players to the newly connected client
        auto view = _world.registry.view<NetworkedEntity, TransformComponent, PlayerComponent>();
        for (auto entity : view) {
            auto& networked = view.get<NetworkedEntity>(entity);
            auto& transform = view.get<TransformComponent>(entity);

            // Skip the newly spawned player (already sent above)
            if (networked.owner_client_id == client_id) {
                continue;
            }

            // Send SPAWN_PLAYER for this existing player
            SpawnPlayerMessage existing_msg;
            existing_msg.client_id = networked.owner_client_id;
            existing_msg.entity_id = networked.network_id;
            existing_msg.position = transform.position;
            existing_msg.camera_yaw = 0.0f;

            BitWriter existing_writer;
            NetworkSerializer::serialize(existing_writer, existing_msg);
            _network.sendReliableToClient(client_id, existing_writer);
        }

        LOG_ENGINE_INFO("Spawned player entity (network_id={0}) for client {1} at position {2},{3},{4}",
            network_id, client_id, spawn_pos.X, spawn_pos.Y, spawn_pos.Z);
    });

    _network.setOnClientDisconnected([&](uint16_t client_id) {
        LOG_ENGINE_INFO("Client {0} disconnected, despawning player", client_id);

        // Find and destroy player entity for this client
        auto view = _world.registry.view<NetworkedEntity>();
        for (auto entity : view) {
            auto& networked = view.get<NetworkedEntity>(entity);
            if (networked.owner_client_id == client_id && networked.is_player) {
                _network.unregisterEntity(entity);
                _world.registry.destroy(entity);
                LOG_ENGINE_INFO("Despawned player entity for client {0}", client_id);
                break;
            }
        }
    });

    // Initialize application with Headless render API
    app = Application(1280, 720, 60, 75.0f, RenderAPIType::Headless);
    if (!app.initialize("Server", false))
    {
        LOG_ENGINE_FATAL("Failed to initialize Server Application");
        return 1;
    }

    IRenderAPI* render_api = app.getRenderAPI();

    /* Create world */
    _world = world();
    _network.setWorld(&_world);

    /* Level loading */
    LevelManager level_manager;
    LevelData level_data;
    std::string level_path = "levels/main.level.json";

    LOG_ENGINE_INFO("Loading level: {0}", level_path.c_str());
    if (!level_manager.loadLevel(level_path, level_data))
    {
        LOG_ENGINE_FATAL("Failed to load level");
        shutdown_server(1);
    }

    if (!level_manager.instantiateLevel(level_data, _world, render_api))
    {
        LOG_ENGINE_FATAL("Failed to instantiate level");
        shutdown_server(1);
    }

    LOG_ENGINE_INFO("Server started successfully");

    /* Delta time */
    Uint32 delta_last = SDL_GetTicks();
    float delta_time = 0;

    bool running = true;
    while (running)
    {
        Uint32 frame_start_ticks = SDL_GetTicks();
        delta_time = (frame_start_ticks - delta_last) / 1000.0f;
        delta_last = frame_start_ticks;

        // Network update
        _network.update(delta_time);

        // Physics step (Server is authority)
        _world.step_physics();

        // Broadcast state to all clients
        _network.broadcastWorldState();

        // Server-only game rules
        game_rules.Update(_world, delta_time);

        // Limit server tick rate
        Uint32 frame_end_ticks = SDL_GetTicks();
        app.lockFramerate(frame_start_ticks, frame_end_ticks);

        // Simple exit condition (could be replaced with signal handling)
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }
    }

    shutdown_server(0);
    return 0;
}