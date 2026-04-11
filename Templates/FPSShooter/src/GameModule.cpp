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
#include "shared/WeaponTypes.hpp"
#include "shared/WeaponSystem.hpp"
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

#include <SDL3/SDL.h>
#include <memory>

// ---- Module state ----

static EngineServices* g_services = nullptr;
static std::unique_ptr<PlayerController> g_player_controller;
static Game::ClientNetworkManager g_network;
static GameHUD g_hud;

static entt::entity g_player_entity = entt::null;
static entt::entity g_freecam_entity = entt::null;
static entt::entity g_player_rep_entity = entt::null;

// Combat state
static ReconciliationSmoothing g_recon_smoothing;
static int32_t g_local_health = 100;
static int32_t g_local_max_health = 100;
static bool g_local_alive = true;
static int32_t g_local_ammo = 30;
static int32_t g_local_max_ammo = 30;
static bool g_local_reloading = false;
static float g_local_fire_cooldown = 0.0f;
static int32_t g_local_kills = 0;
static int32_t g_local_deaths = 0;
static float g_death_timer = 0.0f; // Countdown shown on death screen
static constexpr float RESPAWN_DELAY = 3.0f;

// Kill feed entries
struct KillFeedEntry {
    std::string killer_name;
    std::string victim_name;
    float timer = 5.0f;
};
static std::vector<KillFeedEntry> g_kill_feed;

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

    // Set up combat callbacks
    g_network.setOnShootResult([](const ShootResultMessage& msg) {
        // Draw tracer line for visual feedback
        glm::vec3 color = (msg.hit_entity_id != 0) ? glm::vec3(1.0f, 0.3f, 0.0f) : glm::vec3(1.0f, 1.0f, 0.5f);
        DebugDraw::get().addLine(msg.ray_origin, msg.hit_position, color, 0.15f);
    });

    g_network.setOnDamageEvent([](const DamageEventMessage& msg) {
        if (msg.victim_client_id == g_network.getClientId()) {
            g_local_health = msg.health_remaining;
            LOG_ENGINE_INFO("Took {} damage! Health: {}", msg.damage, g_local_health);
        }
    });

    g_network.setOnPlayerDied([](const PlayerDiedMessage& msg) {
        // Add kill feed entry
        KillFeedEntry entry;
        entry.killer_name = "Player " + std::to_string(msg.killer_client_id);
        entry.victim_name = "Player " + std::to_string(msg.victim_client_id);
        entry.timer = 5.0f;
        g_kill_feed.push_back(entry);

        if (msg.victim_client_id == g_network.getClientId()) {
            g_local_alive = false;
            g_local_health = 0;
            g_local_deaths++;
            g_death_timer = RESPAWN_DELAY;
            LOG_ENGINE_INFO("You died! Killed by Player {}", msg.killer_client_id);
        }
        if (msg.killer_client_id == g_network.getClientId() &&
            msg.killer_client_id != msg.victim_client_id) {
            g_local_kills++;
        }
    });

    g_network.setOnPlayerRespawn([](const PlayerRespawnMessage& msg) {
        if (msg.client_id == g_network.getClientId()) {
            g_local_alive = true;
            g_local_health = msg.health;
            g_local_max_health = msg.health;
            g_death_timer = 0.0f;
            g_local_fire_cooldown = 0.0f;
            g_local_reloading = false;

            // Reset weapon
            const auto& def = getWeaponDef(WeaponType::RIFLE);
            g_local_ammo = def.max_ammo;
            g_local_max_ammo = def.max_ammo;

            // Reset reconciliation smoothing
            g_recon_smoothing.visual_offset = glm::vec3(0.0f);

            LOG_ENGINE_INFO("Respawned with {} health", msg.health);
        }
    });

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

    // Update kill feed timers
    for (auto it = g_kill_feed.begin(); it != g_kill_feed.end(); ) {
        it->timer -= delta_time;
        if (it->timer <= 0.0f) {
            it = g_kill_feed.erase(it);
        } else {
            ++it;
        }
    }

    // Update death timer
    if (!g_local_alive && g_death_timer > 0.0f) {
        g_death_timer -= delta_time;
    }

    // Update local weapon cooldowns
    if (g_local_fire_cooldown > 0.0f) g_local_fire_cooldown -= delta_time;

    // Send player input to server and run client-side prediction
    if (g_network.isConnected() && input_manager)
    {
        // Disable PlayerController's built-in movement — SharedMovement handles it
        g_player_controller->setMovementEnabled(false);

        Game::InputState input_state;
        input_state.buttons = 0;

        if (g_local_alive) {
            if (input_manager->is_key_held(SDL_SCANCODE_W)) input_state.buttons |= InputFlags::MOVE_FORWARD;
            if (input_manager->is_key_held(SDL_SCANCODE_S)) input_state.buttons |= InputFlags::MOVE_BACK;
            if (input_manager->is_key_held(SDL_SCANCODE_A)) input_state.buttons |= InputFlags::MOVE_LEFT;
            if (input_manager->is_key_held(SDL_SCANCODE_D)) input_state.buttons |= InputFlags::MOVE_RIGHT;
            if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) input_state.buttons |= InputFlags::JUMP;
            if (input_manager->is_key_held(SDL_SCANCODE_E)) input_state.buttons |= InputFlags::USE;
        }

        input_state.camera_yaw = game_world->world_camera.rotation.y;
        input_state.camera_pitch = game_world->world_camera.rotation.x;
        input_state.move_forward = 0.0f;
        input_state.move_right = 0.0f;

        if (input_state.buttons & InputFlags::MOVE_FORWARD) input_state.move_forward += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_BACK) input_state.move_forward -= 1.0f;
        if (input_state.buttons & InputFlags::MOVE_RIGHT) input_state.move_right += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_LEFT) input_state.move_right -= 1.0f;

        // Handle shooting (Mouse1 / ATTACK)
        if (g_local_alive && input_manager->is_mouse_button_held(1) && g_local_fire_cooldown <= 0.0f && !g_local_reloading && g_local_ammo > 0)
        {
            const auto& weapon_def = getWeaponDef(WeaponType::RIFLE);

            // Consume ammo locally (client prediction)
            g_local_ammo--;
            g_local_fire_cooldown = weapon_def.fire_rate;

            // Auto-reload
            if (g_local_ammo <= 0) {
                g_local_reloading = true;
            }

            // Send shoot command to server
            glm::vec3 cam_pos = game_world->world_camera.getPosition();
            glm::vec3 cam_fwd = game_world->world_camera.camera_forward();
            g_network.sendShootCommand(cam_pos, cam_fwd, static_cast<uint8_t>(WeaponType::RIFLE));
        }

        // Handle reload (R key)
        if (g_local_alive && input_manager->is_key_pressed(SDL_SCANCODE_R) && !g_local_reloading) {
            const auto& weapon_def = getWeaponDef(WeaponType::RIFLE);
            if (g_local_ammo < weapon_def.max_ammo) {
                g_local_reloading = true;
                // Reload timer handled by server, client just shows the state
            }
        }

        // Client-side prediction for movement
        if (g_local_alive &&
            game_world->registry.valid(g_player_entity) &&
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

            // Server reconciliation with smooth visual correction
            MovementState server_state;
            uint32_t server_tick;
            if (g_network.popAuthoritativeUpdate(server_state, server_tick))
            {
                // Save current display position before correction
                glm::vec3 old_display_pos = g_recon_smoothing.getDisplayPosition(trans.position);

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

                    // Apply smooth visual correction instead of snap
                    g_recon_smoothing.onCorrection(old_display_pos, trans.position);
                }
            }

            // Decay visual smoothing offset each frame
            g_recon_smoothing.update();
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

    // Apply visual smoothing offset to camera position
    if (g_network.isConnected() && game_world->registry.valid(g_player_entity) &&
        game_world->registry.all_of<TransformComponent>(g_player_entity))
    {
        auto& trans = game_world->registry.get<TransformComponent>(g_player_entity);
        glm::vec3 display_pos = g_recon_smoothing.getDisplayPosition(trans.position);
        // Offset camera by the smoothing delta
        glm::vec3 offset = display_pos - trans.position;
        game_world->world_camera.position += offset;
    }

    // Update player representations
    update_player_representations(game_world->registry,
        g_player_controller ? g_player_controller->isFreecamMode() : false);

    // Fall detection (client-side, for offline mode)
    if (!g_network.isConnected() && g_player_controller && !g_player_controller->isFreecamMode() &&
        game_world->registry.valid(g_player_entity))
    {
        auto& t = game_world->registry.get<TransformComponent>(g_player_entity);
        if (t.position.y < -50)
        {
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

        // Build kill feed text (last 5 entries)
        std::string kill_feed_text;
        int count = 0;
        for (auto it = g_kill_feed.rbegin(); it != g_kill_feed.rend() && count < 5; ++it, ++count) {
            if (!kill_feed_text.empty()) kill_feed_text += "\n";
            kill_feed_text += it->killer_name + " killed " + it->victim_name;
        }

        g_hud.update(fps, pos, speed, grounded, g_network.isConnected(),
                     g_network.isConnected() ? g_network.getStats().ping_ms : 0.0f,
                     g_local_health, g_local_max_health, g_local_ammo, g_local_max_ammo,
                     g_local_alive, g_death_timer, g_local_kills, g_local_deaths,
                     kill_feed_text, g_local_reloading);
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

    // Reset combat state
    g_local_health = 100;
    g_local_max_health = 100;
    g_local_alive = true;
    g_local_kills = 0;
    g_local_deaths = 0;
    g_death_timer = 0.0f;
    g_recon_smoothing.visual_offset = glm::vec3(0.0f);
    g_kill_feed.clear();

    const auto& def = getWeaponDef(WeaponType::RIFLE);
    g_local_ammo = def.max_ammo;
    g_local_max_ammo = def.max_ammo;
    g_local_reloading = false;
    g_local_fire_cooldown = 0.0f;
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
