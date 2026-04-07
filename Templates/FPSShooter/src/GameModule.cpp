#include "Plugin/GameModuleAPI.h"
#include "Reflection/Reflect.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/SharedMovement.hpp"
#include "ClientNetworkManager.hpp"
#include "GameHUD.hpp"
#include "PlayerRepSystem.hpp"

#include "Utils/Log.hpp"
#include "Assets/AssetManager.hpp"
#include "UI/RmlUiManager.h"
#include "Console/ConVar.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Timer/TimerSystem.hpp"
#include "Debug/DebugDraw.hpp"
#include "Audio/AudioSystem.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Threading/JobSystem.hpp"
#include "Assets/AssetManager.hpp"

#include <SDL.h>
#include <memory>

// ---- Module state ----

static EngineServices* g_services = nullptr;
static std::unique_ptr<PlayerController> g_player_controller;
static Game::ClientNetworkManager g_network;
static GameHUD g_hud;

static entt::entity g_player_entity = entt::null;
static entt::entity g_freecam_entity = entt::null;
static entt::entity g_player_rep_entity = entt::null;

// ---- DLL exports ----

GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}

GAME_API const char* gardenGetGameName()
{
    return "FPSShooter";
}

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;

    // Initialize networking
    if (!g_network.initialize()) {
        LOG_ENGINE_FATAL("Failed to initialize Client Network");
        return false;
    }

    const char* address = (services->connect_address && services->connect_address[0])
                          ? services->connect_address : "127.0.0.1";
    uint16_t port = services->connect_port ? services->connect_port : 7777;

    if (!g_network.connectToServer(address, port, "Player")) {
        LOG_ENGINE_WARN("Failed to connect to server at {}:{} - running in offline mode", address, port);
    }

    g_network.setWorld(services->game_world);

    // Initialize HUD
    if (RmlUiManager::get().isInitialized())
    {
        if (!g_hud.initialize(RmlUiManager::get().getContext(),
                Assets::AssetManager::get().resolveAssetPath("ui/hud.rml")))
        {
            LOG_ENGINE_WARN("Failed to load HUD document");
        }
    }

    // Create player controller
    g_player_controller = std::make_unique<PlayerController>(
        services->input_manager ? std::shared_ptr<InputManager>(services->input_manager, [](InputManager*){}) : nullptr,
        services->game_world);

    return true;
}

GAME_API void gardenGameShutdown()
{
    ConVarRegistry::get().saveArchiveCvars("config.cfg");
    AudioSystem::get().shutdown();
    g_network.disconnect("Game closing");
    g_network.shutdown();
    g_player_controller.reset();
    g_hud.shutdown();
    g_services = nullptr;
}

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
    // Register game-specific components here
}

GAME_API void gardenGameUpdate(float delta_time)
{
    if (!g_services) return;

    world* game_world = g_services->game_world;
    auto input_manager = g_services->input_manager;

    // Flush deferred events from previous frame
    EventBus::get().flush();

    // Update timers
    TimerSystem::get().update(delta_time);

    // Update debug draw
    DebugDraw::get().update(delta_time);

    // Network update
    g_network.update(delta_time);

    // Send player input to server and run client-side prediction
    if (g_network.isConnected() && input_manager)
    {
        // Disable PlayerController's built-in movement — SharedMovement handles it
        g_player_controller->setMovementEnabled(false);

        Game::InputState input_state;
        input_state.buttons = 0;

        if (input_manager->is_key_held(SDL_SCANCODE_W)) input_state.buttons |= InputFlags::MOVE_FORWARD;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) input_state.buttons |= InputFlags::MOVE_BACK;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) input_state.buttons |= InputFlags::MOVE_LEFT;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) input_state.buttons |= InputFlags::MOVE_RIGHT;
        if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) input_state.buttons |= InputFlags::JUMP;
        if (input_manager->is_key_held(SDL_SCANCODE_E)) input_state.buttons |= InputFlags::USE;

        input_state.camera_yaw = game_world->world_camera.rotation.y;
        input_state.camera_pitch = game_world->world_camera.rotation.x;
        input_state.move_forward = 0.0f;
        input_state.move_right = 0.0f;

        if (input_state.buttons & InputFlags::MOVE_FORWARD) input_state.move_forward += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_BACK) input_state.move_forward -= 1.0f;
        if (input_state.buttons & InputFlags::MOVE_RIGHT) input_state.move_right += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_LEFT) input_state.move_right -= 1.0f;

        // Client-side prediction
        if (game_world->registry.valid(g_player_entity) &&
            game_world->registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(g_player_entity))
        {
            auto& trans = game_world->registry.get<TransformComponent>(g_player_entity);
            auto& rb = game_world->registry.get<RigidBodyComponent>(g_player_entity);
            auto& pc = game_world->registry.get<PlayerComponent>(g_player_entity);

            MovementInput move_input;
            move_input.move_forward = input_state.move_forward;
            move_input.move_right = input_state.move_right;
            move_input.camera_yaw = input_state.camera_yaw;
            move_input.camera_pitch = input_state.camera_pitch;
            move_input.buttons = input_state.buttons;

            MovementState move_state;
            move_state.position = trans.position;
            move_state.velocity = rb.velocity;
            move_state.grounded = pc.grounded;
            move_state.ground_normal = pc.ground_normal;

            MovementConfig move_config;
            move_config.speed = pc.speed;
            move_config.jump_force = pc.jump_force;
            move_config.fixed_delta = game_world->fixed_delta;

            MovementState result = SharedMovement::simulate(move_input, move_state, move_config);

            uint32_t prediction_tick = g_network.getClientTick();
            g_network.storeInput(prediction_tick, move_input, result);

            trans.position = result.position;
            rb.velocity = result.velocity;
            pc.grounded = result.grounded;

            // Server reconciliation
            MovementState server_state;
            uint32_t server_tick;
            if (g_network.popAuthoritativeUpdate(server_state, server_tick))
            {
                float pos_error = glm::distance(server_state.position, trans.position);
                if (pos_error > 0.01f)
                {
                    MovementState replay_state = server_state;
                    g_network.getInputHistory().forEachFrom(server_tick, [&](const PredictionEntry& entry) {
                        replay_state.grounded = entry.predicted_state.grounded;
                        replay_state.ground_normal = entry.predicted_state.ground_normal;
                        replay_state = SharedMovement::simulate(entry.input, replay_state, move_config);
                    });

                    trans.position = replay_state.position;
                    rb.velocity = replay_state.velocity;
                    pc.grounded = replay_state.grounded;
                    pc.ground_normal = replay_state.ground_normal;
                }
            }
        }

        g_network.sendInputCommand(input_state);
    }
    else
    {
        if (g_player_controller)
            g_player_controller->setMovementEnabled(true);
    }

    // Physics and player collisions
    if (g_player_controller && !g_player_controller->isFreecamMode())
    {
        game_world->step_physics(delta_time);
        game_world->player_collisions(g_player_entity);
    }

    // Update player controller
    if (g_player_controller)
        g_player_controller->update(delta_time);

    // Update player representations
    update_player_representations(game_world->registry,
        g_player_controller ? g_player_controller->isFreecamMode() : false);

    // Fall detection
    if (g_player_controller && !g_player_controller->isFreecamMode() &&
        game_world->registry.valid(g_player_entity))
    {
        auto& t = game_world->registry.get<TransformComponent>(g_player_entity);
        if (t.position.y < -50)
        {
            // Respawn at origin instead of quitting
            t.position = glm::vec3(0, 5, 0);
            if (game_world->registry.all_of<RigidBodyComponent>(g_player_entity))
                game_world->registry.get<RigidBodyComponent>(g_player_entity).velocity = glm::vec3(0);
        }
    }

    // Update animations
    AnimationSystem::update(game_world->registry, delta_time);

    // Update audio listener
    if (g_player_controller)
    {
        camera& active_camera = g_player_controller->getActiveCamera();
        AudioSystem::get().setListenerPosition(
            active_camera.getPosition(),
            active_camera.camera_forward(),
            active_camera.getUpVector());
    }
    AudioSystem::get().update();

    // Interpolate remote entities
    if (g_network.isConnected())
        g_network.interpolateRemoteEntities();

    // Update HUD
    {
        float fps = (delta_time > 0.0f) ? 1.0f / delta_time : 0.0f;
        glm::vec3 pos(0.0f);
        float speed = 0.0f;
        bool grounded = false;
        if (game_world->registry.valid(g_player_entity))
        {
            auto& t = game_world->registry.get<TransformComponent>(g_player_entity);
            pos = t.position;
            if (game_world->registry.all_of<RigidBodyComponent>(g_player_entity))
                speed = glm::length(game_world->registry.get<RigidBodyComponent>(g_player_entity).velocity);
            if (game_world->registry.all_of<PlayerComponent>(g_player_entity))
                grounded = game_world->registry.get<PlayerComponent>(g_player_entity).grounded;
        }
        g_hud.update(fps, pos, speed, grounded, g_network.isConnected(),
                     g_network.isConnected() ? g_network.getStats().ping_ms : 0.0f);
    }
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services) return;

    world* game_world = g_services->game_world;

    // Find player, freecam, and player rep entities
    g_player_entity = entt::null;
    g_freecam_entity = entt::null;
    g_player_rep_entity = entt::null;

    auto view = game_world->registry.view<TagComponent>();
    for (auto entity : view)
    {
        // Find by component type
        if (game_world->registry.all_of<PlayerComponent>(entity) && g_player_entity == entt::null)
            g_player_entity = entity;
        if (game_world->registry.all_of<FreecamComponent>(entity) && g_freecam_entity == entt::null)
            g_freecam_entity = entity;
        if (game_world->registry.all_of<PlayerRepresentationComponent>(entity) && g_player_rep_entity == entt::null)
            g_player_rep_entity = entity;
    }

    // Optimize broad phase after level load
    game_world->getPhysicsSystem().optimizeBroadPhase();

    // Set up player controller
    if (g_player_controller)
    {
        g_player_controller->setPossessedPlayer(g_player_entity);
        g_player_controller->setPossessedFreecam(g_freecam_entity);
    }

    // Set camera to player position
    if (game_world->registry.valid(g_player_entity))
    {
        auto& t = game_world->registry.get<TransformComponent>(g_player_entity);
        game_world->world_camera.position = t.position;
        game_world->world_camera.rotation = t.rotation;
    }
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
