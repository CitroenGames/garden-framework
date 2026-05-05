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

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

static EngineServices* g_server_services = nullptr;
static Net::ServerNetworkManager g_server_network;
static Game::GameRules g_game_rules;

using namespace Net;

static bool ensurePlayerPhysicsBody(world& w, entt::entity player_entity)
{
    if (!w.registry.valid(player_entity))
        return false;
    if (!w.registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(player_entity))
        return false;

    JPH::BodyID body_id = w.getPhysicsSystem().createPlayerBody(w.registry, player_entity);
    if (body_id.IsInvalid())
    {
        LOG_ENGINE_ERROR("Failed to create server player physics body");
        return false;
    }
    return true;
}

static void updateServerPlayerCollisions(world& w)
{
    auto view = w.registry.view<NetworkedEntity, PlayerComponent, RigidBodyComponent, TransformComponent>();
    for (auto entity : view)
    {
        const auto& net = view.get<NetworkedEntity>(entity);
        if (!net.is_player)
            continue;
        w.player_collisions(entity);
    }
}

// ---- Hit validation helpers ----

// Tests a ray against a vertical capsule centered at player position.
static bool rayCapsuleIntersect(const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                                const glm::vec3& capsule_center, float capsule_radius,
                                float capsule_height, glm::vec3& hit_point)
{
    const glm::vec3 axis(0.0f, 1.0f, 0.0f);
    const glm::vec3 seg_a = capsule_center - axis * (capsule_height * 0.5f);
    const glm::vec3 seg_b = capsule_center + axis * (capsule_height * 0.5f);
    const glm::vec3 seg = seg_b - seg_a;
    const glm::vec3 w0 = ray_origin - seg_a;

    const float a = glm::dot(ray_dir, ray_dir);
    const float b = glm::dot(ray_dir, seg);
    const float c = glm::dot(seg, seg);
    const float d = glm::dot(ray_dir, w0);
    const float e = glm::dot(seg, w0);
    const float denom = a * c - b * b;

    float ray_t = 0.0f;
    float seg_t = 0.0f;
    if (denom > 0.000001f) {
        ray_t = (b * e - c * d) / denom;
        seg_t = (a * e - b * d) / denom;
    } else if (c > 0.000001f) {
        seg_t = e / c;
    }

    seg_t = std::clamp(seg_t, 0.0f, 1.0f);
    ray_t = (b * seg_t - d) / a;
    if (ray_t < 0.0f) {
        ray_t = 0.0f;
        seg_t = c > 0.000001f ? std::clamp(e / c, 0.0f, 1.0f) : 0.0f;
    }

    const glm::vec3 closest_ray = ray_origin + ray_dir * ray_t;
    const glm::vec3 closest_capsule = seg_a + seg * seg_t;
    if (glm::distance(closest_ray, closest_capsule) > capsule_radius) {
        return false;
    }

    hit_point = closest_ray;
    return true;
}

static glm::vec3 makeAimDirection(float pitch, float yaw)
{
    const glm::vec3 clamped_rotation(std::clamp(pitch, -1.0f, 1.0f), yaw, 0.0f);
    return glm::normalize(glm::quat(clamped_rotation) * glm::vec3(0.0f, 0.0f, 1.0f));
}

static glm::vec3 getShootOrigin(const TransformComponent& transform)
{
    return transform.position;
}

static WeaponStateMessage makeWeaponStateMessage(const WeaponComponent& weapon)
{
    WeaponStateMessage msg;
    msg.ammo = weapon.ammo;
    msg.max_ammo = weapon.max_ammo;
    msg.weapon_type = static_cast<uint8_t>(weapon.weapon_type);
    msg.reloading = weapon.reloading ? 1 : 0;
    msg.fire_cooldown = weapon.fire_cooldown;
    msg.reload_timer = weapon.reload_timer;
    return msg;
}

static void sendWeaponState(uint16_t client_id, const WeaponComponent& weapon)
{
    BitWriter writer;
    CombatSerializer::serialize(writer, makeWeaponStateMessage(weapon));
    g_server_network.sendReliableToClient(client_id, writer);
}

static uint32_t getLagCompensatedAimTick(uint32_t acknowledged_server_tick)
{
    if (acknowledged_server_tick <= DEFAULT_INTERP_DELAY_TICKS) {
        return acknowledged_server_tick;
    }
    return acknowledged_server_tick - DEFAULT_INTERP_DELAY_TICKS;
}

// Process an authoritative attack input sample with lag compensation.
static void fireServerShot(
    uint16_t shooter_client_id,
    entt::entity shooter_entity,
    const InputSample& input,
    uint32_t acknowledged_server_tick,
    const WeaponComponent& weapon)
{
    world* w = g_server_services->game_world;

    const auto& shooter_transform = w->registry.get<TransformComponent>(shooter_entity);
    const auto& weapon_def = getWeaponDef(weapon.weapon_type);
    const glm::vec3 ray_origin = getShootOrigin(shooter_transform);
    const glm::vec3 ray_dir = makeAimDirection(input.camera_pitch, input.camera_yaw);

    const uint32_t rewind_tick = getLagCompensatedAimTick(acknowledged_server_tick);

    // Find best hit across all pellets
    uint32_t best_hit_entity_id = 0;
    uint16_t best_hit_client_id = 0;
    glm::vec3 best_hit_point = ray_origin + ray_dir * weapon_def.range;
    float best_hit_dist = weapon_def.range;
    entt::entity best_hit_ent = entt::null;
    auto player_view = w->registry.view<NetworkedEntity, TransformComponent, HealthComponent>();

    // For each pellet
    for (int pellet = 0; pellet < weapon_def.pellets; pellet++) {
        glm::vec3 pellet_dir = WeaponSystem::applySpread(ray_dir, weapon_def.spread,
            input.tick * 31 + pellet * 7919 + shooter_client_id);

        for (auto entity : player_view) {
            if (entity == shooter_entity) continue;

            auto& net = player_view.get<NetworkedEntity>(entity);
            auto& health = player_view.get<HealthComponent>(entity);
            auto& trans = player_view.get<TransformComponent>(entity);

            if (!health.alive) continue;

            glm::vec3 target_position = trans.position;
            ComponentSnapshot rewind_snapshot;
            if (g_server_network.sampleLagCompensatedEntity(net.network_id, rewind_tick, rewind_snapshot)) {
                target_position = rewind_snapshot.position;
            }

            // Player bounding capsule: radius=0.4m, height=1.8m
            glm::vec3 hit_point;
            if (rayCapsuleIntersect(ray_origin, pellet_dir, target_position, 0.4f, 1.8f, hit_point)) {
                float dist = glm::distance(ray_origin, hit_point);
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
    result_msg.ray_origin = ray_origin;
    result_msg.hit_position = best_hit_point;
    result_msg.hit_entity_id = best_hit_entity_id;
    result_msg.weapon_type = static_cast<uint8_t>(weapon.weapon_type);

    BitWriter result_writer;
    CombatSerializer::serialize(result_writer, result_msg);

    for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
        const ClientInfo* client = g_server_network.getClientInfo(i);
        if (client && client->peer)
            g_server_network.sendReliableToClient(i, result_writer);
    }
}

static void processWeaponInput(
    uint16_t client_id,
    entt::entity player_entity,
    const InputSample& input,
    uint32_t acknowledged_server_tick)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.valid(player_entity)) {
        return;
    }

    if (!w->registry.all_of<TransformComponent, HealthComponent, WeaponComponent>(player_entity)) {
        return;
    }

    auto& health = w->registry.get<HealthComponent>(player_entity);
    if (!health.alive) {
        return;
    }

    auto& weapon = w->registry.get<WeaponComponent>(player_entity);
    const bool wants_attack = (input.buttons & InputFlags::ATTACK) != 0;
    const bool wants_reload = (input.buttons & InputFlags::RELOAD) != 0;
    if (!wants_attack && !wants_reload) {
        return;
    }

    if (wants_attack) {
        if (WeaponSystem::tryFire(weapon)) {
            fireServerShot(client_id, player_entity, input, acknowledged_server_tick, weapon);
        }
    } else if (wants_reload) {
        (void)WeaponSystem::tryReload(weapon);
    }
    sendWeaponState(client_id, weapon);
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
                auto& rb = w->registry.get<RigidBodyComponent>(entity);
                rb.velocity = glm::vec3(0.0f);
                rb.force = glm::vec3(0.0f);
            }

            ensurePlayerPhysicsBody(*w, entity);

            if (w->registry.all_of<WeaponComponent>(entity)) {
                auto& weapon = w->registry.get<WeaponComponent>(entity);
                initWeapon(weapon, WeaponType::RIFLE);
                sendWeaponState(client_id, weapon);
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
    rigidbody.velocity = glm::vec3(0.0f);
    rigidbody.force = glm::vec3(0.0f);
    rigidbody.mass = 80.0f;
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
    auto& weapon_component = w->registry.emplace<WeaponComponent>(player_entity, weapon);

    w->registry.emplace<ScoreComponent>(player_entity);

    ensurePlayerPhysicsBody(*w, player_entity);

    g_server_network.setClientPlayerEntity(client_id, network_id);
    sendWeaponState(client_id, weapon_component);

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
            w->getPhysicsSystem().removeBody(entity);

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

    g_server_network.setInputSampleHandler([](
        uint16_t client_id,
        entt::entity player_entity,
        const InputSample& input,
        uint32_t acknowledged_server_tick) {
        processWeaponInput(client_id, player_entity, input, acknowledged_server_tick);
    });

    g_server_network.setOnClientConnected([](uint16_t client_id) {
        spawnPlayerForClient(client_id);
    });

    g_server_network.setOnClientDisconnected([](uint16_t client_id) {
        despawnPlayerForClient(client_id);
    });

    g_server_network.setCustomMessageHandler([](uint16_t client_id, uint8_t message_type, BitReader& reader) {
        (void)reader;
        LOG_ENGINE_WARN("Ignoring unauthoritative client custom message {} from client {}",
            static_cast<int>(message_type), client_id);
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

    g_server_network.pumpNetworkEvents(delta_time);
    w->step_physics(delta_time);
    updateServerPlayerCollisions(*w);
    g_game_rules.Update(*w, delta_time);

    // Update weapon cooldowns for all players
    auto weapon_view = w->registry.view<WeaponComponent, NetworkedEntity>();
    for (auto entity : weapon_view) {
        auto& weapon = weapon_view.get<WeaponComponent>(entity);
        auto& net = weapon_view.get<NetworkedEntity>(entity);
        const int32_t old_ammo = weapon.ammo;
        const bool old_reloading = weapon.reloading;
        WeaponSystem::tick(weapon, w->fixed_delta);
        if ((old_ammo != weapon.ammo || old_reloading != weapon.reloading) && net.owner_client_id != 0) {
            sendWeaponState(net.owner_client_id, weapon);
        }
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

    g_server_network.publishWorldState();
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
