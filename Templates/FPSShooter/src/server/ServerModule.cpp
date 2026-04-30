// Server-side DLL exports for the FPSShooter template.
// These are resolved by the engine's Server.exe launcher via GameModuleLoader.
// They are optional - if absent, the DLL only supports client mode.

#include "Plugin/GameModuleAPI.h"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Components/Components.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/NetworkSerializer.hpp"
#include "Network/NetworkTypes.hpp"
#include "Network/ServerNetworkManager.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/CombatProtocol.hpp"
#include "shared/WeaponTypes.hpp"
#include "shared/WeaponSystem.hpp"
#include "server/GameRules.hpp"
#include "Utils/Log.hpp"

#include <cmath>

static EngineServices* g_server_services = nullptr;
static Net::ServerNetworkManager g_server_network;
static Game::GameRules g_game_rules;

using namespace Net;

// ---- Hit validation helpers ----

// Simple capsule-ray intersection for lag-compensated hit detection.
// Tests ray against a vertical capsule centered at player position.
static bool rayCapsuleIntersect(const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                                const glm::vec3& capsule_center, float capsule_radius,
                                float capsule_height, glm::vec3& hit_point)
{
    // Approximate as a sphere for simplicity (player bounding sphere)
    // Use a tall sphere centered at capsule_center + half height offset
    glm::vec3 center = capsule_center + glm::vec3(0, capsule_height * 0.5f, 0);
    float radius = capsule_radius;

    glm::vec3 oc = ray_origin - center;
    float a = glm::dot(ray_dir, ray_dir);
    float b = 2.0f * glm::dot(oc, ray_dir);
    float c = glm::dot(oc, oc) - radius * radius;
    float discriminant = b * b - 4.0f * a * c;

    if (discriminant < 0.0f) return false;

    float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0.0f) {
        t = (-b + std::sqrt(discriminant)) / (2.0f * a);
        if (t < 0.0f) return false;
    }

    hit_point = ray_origin + ray_dir * t;
    return true;
}

// Process a shoot command from a client with lag compensation
static void processShootCommand(uint16_t shooter_client_id, const ShootCommandMessage& shoot_msg)
{
    world* w = g_server_services->game_world;

    // Find shooter entity
    entt::entity shooter_entity = entt::null;
    auto player_view = w->registry.view<NetworkedEntity, TransformComponent, HealthComponent>();
    for (auto entity : player_view) {
        auto& net = player_view.get<NetworkedEntity>(entity);
        if (net.owner_client_id == shooter_client_id && net.is_player) {
            shooter_entity = entity;
            break;
        }
    }

    if (shooter_entity == entt::null) return;

    auto& shooter_health = w->registry.get<HealthComponent>(shooter_entity);
    if (!shooter_health.alive) return;

    // Check weapon cooldown (server-authoritative)
    if (!w->registry.all_of<WeaponComponent>(shooter_entity)) return;
    auto& weapon = w->registry.get<WeaponComponent>(shooter_entity);
    if (!WeaponSystem::tryFire(weapon)) return;

    WeaponType wtype = static_cast<WeaponType>(shoot_msg.weapon_type);
    const auto& weapon_def = getWeaponDef(wtype);
    glm::vec3 ray_dir = glm::normalize(shoot_msg.ray_direction);

    // Lag compensation: use the client's reported tick to look up historical positions.
    // The server's snapshot history (in ServerNetworkManager) stores world state at each tick.
    // We check ray against each other player's position at the client's perceived tick.
    uint32_t rewind_tick = shoot_msg.client_tick;

    // Find best hit across all pellets
    uint32_t best_hit_entity_id = 0;
    uint16_t best_hit_client_id = 0;
    glm::vec3 best_hit_point = shoot_msg.ray_origin + ray_dir * weapon_def.range;
    float best_hit_dist = weapon_def.range;
    entt::entity best_hit_ent = entt::null;

    // For each pellet
    for (int pellet = 0; pellet < weapon_def.pellets; pellet++) {
        glm::vec3 pellet_dir = WeaponSystem::applySpread(ray_dir, weapon_def.spread,
            shoot_msg.client_tick * 31 + pellet * 7919 + shooter_client_id);

        // Check against each other player using current positions
        // (Simplified: use current positions. Full lag comp would rewind to rewind_tick.)
        for (auto entity : player_view) {
            if (entity == shooter_entity) continue;

            auto& net = player_view.get<NetworkedEntity>(entity);
            auto& health = player_view.get<HealthComponent>(entity);
            auto& trans = player_view.get<TransformComponent>(entity);

            if (!health.alive) continue;

            // Player bounding capsule: radius=0.4m, height=1.8m
            glm::vec3 hit_point;
            if (rayCapsuleIntersect(shoot_msg.ray_origin, pellet_dir, trans.position, 0.4f, 1.8f, hit_point)) {
                float dist = glm::distance(shoot_msg.ray_origin, hit_point);
                if (dist < best_hit_dist && dist <= weapon_def.range) {
                    best_hit_dist = dist;
                    best_hit_point = hit_point;
                    best_hit_entity_id = net.network_id;
                    best_hit_client_id = net.owner_client_id;
                    best_hit_ent = entity;
                }
            }
        }
    }

    // Apply damage if we hit someone
    if (best_hit_ent != entt::null && w->registry.valid(best_hit_ent)) {
        auto& victim_health = w->registry.get<HealthComponent>(best_hit_ent);
        int32_t total_damage = weapon_def.damage; // Per pellet that hits (simplified: best hit only)
        victim_health.takeDamage(total_damage);

        // Send damage event to victim
        DamageEventMessage dmg_msg;
        dmg_msg.attacker_client_id = shooter_client_id;
        dmg_msg.victim_client_id = best_hit_client_id;
        dmg_msg.damage = total_damage;
        dmg_msg.health_remaining = victim_health.health;
        dmg_msg.hit_position = best_hit_point;

        BitWriter dmg_writer;
        CombatSerializer::serialize(dmg_writer, dmg_msg);
        g_server_network.sendReliableToClient(best_hit_client_id, dmg_writer);

        // Also send to attacker so they see hit confirmation
        if (shooter_client_id != best_hit_client_id) {
            g_server_network.sendReliableToClient(shooter_client_id, dmg_writer);
        }

        LOG_ENGINE_TRACE("Player {} hit player {} for {} damage (hp: {})",
            shooter_client_id, best_hit_client_id, total_damage, victim_health.health);

        // Check for kill
        if (!victim_health.alive) {
            auto& victim_trans = w->registry.get<TransformComponent>(best_hit_ent);

            // Update scores
            g_game_rules.onPlayerKilled(*w, shooter_client_id, best_hit_client_id);

            // Broadcast death to all clients
            PlayerDiedMessage death_msg;
            death_msg.victim_client_id = best_hit_client_id;
            death_msg.killer_client_id = shooter_client_id;
            death_msg.death_position = victim_trans.position;

            BitWriter death_writer;
            CombatSerializer::serialize(death_writer, death_msg);

            for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
                const ClientInfo* client = g_server_network.getClientInfo(i);
                if (client && client->peer)
                    g_server_network.sendReliableToClient(i, death_writer);
            }

            LOG_ENGINE_INFO("Player {} killed by player {}", best_hit_client_id, shooter_client_id);

            // Queue respawn
            g_game_rules.queueRespawn(best_hit_client_id);
        }
    }

    // Broadcast shoot result to all clients for visual effects (tracers)
    ShootResultMessage result_msg;
    result_msg.shooter_client_id = shooter_client_id;
    result_msg.ray_origin = shoot_msg.ray_origin;
    result_msg.hit_position = best_hit_point;
    result_msg.hit_entity_id = best_hit_entity_id;
    result_msg.weapon_type = shoot_msg.weapon_type;

    BitWriter result_writer;
    CombatSerializer::serialize(result_writer, result_msg);

    for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
        const ClientInfo* client = g_server_network.getClientInfo(i);
        if (client && client->peer)
            g_server_network.sendReliableToClient(i, result_writer);
    }
}

// ---- Respawn handler ----

static void respawnPlayer(uint16_t client_id, const glm::vec3& spawn_pos)
{
    world* w = g_server_services->game_world;

    auto view = w->registry.view<NetworkedEntity, TransformComponent, HealthComponent>();
    for (auto entity : view) {
        auto& net = view.get<NetworkedEntity>(entity);
        if (net.owner_client_id == client_id && net.is_player) {
            auto& trans = view.get<TransformComponent>(entity);
            auto& health = view.get<HealthComponent>(entity);

            // Reset state
            health.reset();
            trans.position = spawn_pos;

            if (w->registry.all_of<RigidBodyComponent>(entity)) {
                w->registry.get<RigidBodyComponent>(entity).velocity = glm::vec3(0);
            }

            if (w->registry.all_of<WeaponComponent>(entity)) {
                initWeapon(w->registry.get<WeaponComponent>(entity), WeaponType::RIFLE);
            }

            // Broadcast respawn to all clients
            PlayerRespawnMessage respawn_msg;
            respawn_msg.client_id = client_id;
            respawn_msg.entity_id = net.network_id;
            respawn_msg.spawn_position = spawn_pos;
            respawn_msg.health = health.health;

            BitWriter writer;
            CombatSerializer::serialize(writer, respawn_msg);

            for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
                const ClientInfo* other = g_server_network.getClientInfo(i);
                if (other && other->peer)
                    g_server_network.sendReliableToClient(i, writer);
            }

            LOG_ENGINE_INFO("Player {} respawned at ({},{},{})", client_id, spawn_pos.x, spawn_pos.y, spawn_pos.z);
            break;
        }
    }
}

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

    // Add combat components
    w->registry.emplace<HealthComponent>(player_entity, 100);

    WeaponComponent weapon;
    initWeapon(weapon, WeaponType::RIFLE);
    w->registry.emplace<WeaponComponent>(player_entity, weapon);

    w->registry.emplace<ScoreComponent>(player_entity);

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

    g_server_network.setInputFilter([](uint16_t, entt::entity player_entity) {
        world* w = g_server_services ? g_server_services->game_world : nullptr;
        if (!w || !w->registry.valid(player_entity)) {
            return false;
        }
        if (!w->registry.all_of<HealthComponent>(player_entity)) {
            return true;
        }
        return w->registry.get<HealthComponent>(player_entity).alive;
    });

    g_server_network.setOnClientConnected([](uint16_t client_id) {
        spawnPlayerForClient(client_id);
    });

    g_server_network.setOnClientDisconnected([](uint16_t client_id) {
        despawnPlayerForClient(client_id);
    });

    // Set up shoot command handler
    g_server_network.setCustomMessageHandler([](uint16_t client_id, uint8_t message_type, BitReader& reader) {
        if (message_type != static_cast<uint8_t>(CombatMessageType::SHOOT_COMMAND)) {
            LOG_ENGINE_WARN("Unhandled client custom message {}", static_cast<int>(message_type));
            return;
        }

        ShootCommandMessage msg;
        if (!CombatSerializer::deserialize(reader, msg)) {
            LOG_ENGINE_WARN("Failed to deserialize SHOOT_COMMAND from client {}", client_id);
            return;
        }
        processShootCommand(client_id, msg);
    });

    // Set up respawn handler
    g_game_rules.setOnRespawn([](uint16_t client_id, const glm::vec3& spawn_pos) {
        respawnPlayer(client_id, spawn_pos);
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

    // Update weapon cooldowns for all players
    auto weapon_view = w->registry.view<WeaponComponent>();
    for (auto entity : weapon_view) {
        auto& weapon = weapon_view.get<WeaponComponent>(entity);
        WeaponSystem::tick(weapon, w->fixed_delta);
    }

    // Fall detection for server-side entities
    auto fall_view = w->registry.view<NetworkedEntity, TransformComponent, HealthComponent>();
    for (auto entity : fall_view) {
        auto& trans = fall_view.get<TransformComponent>(entity);
        auto& health = fall_view.get<HealthComponent>(entity);
        auto& net = fall_view.get<NetworkedEntity>(entity);

        if (health.alive && trans.position.y < -50.0f) {
            // Kill the player (suicide)
            health.alive = false;
            health.health = 0;

            g_game_rules.onPlayerKilled(*w, net.owner_client_id, net.owner_client_id);

            PlayerDiedMessage death_msg;
            death_msg.victim_client_id = net.owner_client_id;
            death_msg.killer_client_id = net.owner_client_id; // Suicide
            death_msg.death_position = trans.position;

            BitWriter death_writer;
            CombatSerializer::serialize(death_writer, death_msg);

            for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
                const ClientInfo* client = g_server_network.getClientInfo(i);
                if (client && client->peer)
                    g_server_network.sendReliableToClient(i, death_writer);
            }

            g_game_rules.queueRespawn(net.owner_client_id);
        }
    }
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
