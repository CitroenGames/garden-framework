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
#include "shared/FpsGameLogic.hpp"
#include "shared/CompetitiveLogic.hpp"
#include "shared/BotLogic.hpp"
#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshPathfinder.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "Navigation/NavMeshGenerator.hpp"
#include <filesystem>
#include "Network/SharedMovement.hpp"
#include "Gameplay/GameplayComponents.hpp"
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "server/GameRules.hpp"
#include "Utils/Log.hpp"
#include "Events/EventBus.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

static EngineServices* g_server_services = nullptr;
static Net::ServerNetworkManager g_server_network;
static Game::GameRules g_game_rules;
// Competitive mode (FPS_GAME_MODE=competitive); DM/TDM paths unchanged when off.
static bool g_competitive = false;
static Competitive::RoundSystem g_round_system;
static Competitive::BuySystem g_buy_system;
static Competitive::EconomySystem g_economy;
static Competitive::BombSystem g_bomb_system;
static Competitive::BombHooksAdapter g_bomb_hooks{&g_bomb_system};
// USE (E, InputFlags::USE) is the plant/defuse channel input: there is no
// separate USE/interact message, and no C4 weapon item, so the assigned
// carrier holding USE in a site plants and a Defender holding USE in range defuses.
static std::unordered_map<uint16_t, bool> g_bomb_use_held;
static std::unordered_map<uint16_t, uint16_t> g_bomb_last_buttons;
static std::unordered_map<uint32_t, int32_t> g_bomb_prev_health;
static std::unordered_map<uint32_t, uint8_t> g_bomb_prev_weapon;

// ---- Bots v1 (FPS_BOTS=N, competitive only; DM/TDM unchanged) ----
struct BotNavMeshResource { Navigation::NavMesh mesh; bool loaded = false; };
static BotNavMeshResource g_bot_nav;
static bool g_bots_attempted = false, g_bots_enabled = false, g_bots_error_logged = false;
static int32_t g_bot_target_count = 0;
static uint32_t g_bot_input_tick = 1;
static float g_bot_world_time = 0.0f;
static std::unordered_map<uint16_t, uint32_t> g_bot_buy_seq;
static std::unordered_map<uint16_t, uint8_t> g_bot_last_weapon_select;
static void spawnPlayerForClient(uint16_t client_id); // defined with player spawning below
static int32_t botCountFromEnv() { const char* v = std::getenv("FPS_BOTS"); return v ? std::max(0, std::atoi(v)) : 0; }
static uint8_t botDifficultyFromEnv() { const char* v = std::getenv("FPS_BOT_DIFFICULTY"); return Bots::clampDifficulty(v ? std::atoi(v) : 1); }
// AI debug v1 (bots.md s10): env switches (FPS_BOTS-style; ConVar plumbing would need
// engine console wiring, env matches existing bot config). Default 0/false = inert.
static int32_t aiDebugLevel() { static const int32_t v = [] { const char* e = std::getenv("AI_DEBUG"); return e ? std::max(0, std::atoi(e)) : 0; }(); return v; }
static bool aiPauseBots() { static const bool v = [] { const char* e = std::getenv("AI_PAUSE_BOTS"); return e && std::atoi(e) != 0; }(); return v; }
static Bots::TransitionRingBuffer g_bot_transitions;
static float g_bot_debug_last_dump = -10.0f;
static std::unordered_map<int32_t, Bots::StuckLogLatch> g_bot_stuck_latches;

static const char* bombStateName(Competitive::BombState st)
{
    using B = Competitive::BombState;
    switch (st) {
    case B::Idle: return "Idle"; case B::Carried: return "Carried"; case B::Dropped: return "Dropped";
    case B::Planting: return "Planting"; case B::Planted: return "Planted"; case B::Defusing: return "Defusing";
    case B::Exploded: return "Exploded"; case B::Defused: return "Defused";
    }
    return "?";
}
static entt::entity g_match_entity = entt::null;
static entt::entity findPlayerEntity(world& w, uint16_t client_id); // defined with competitive helpers below
static void sendEconomyState(uint16_t client_id, entt::entity e);

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
    return transform.position + glm::vec3(0.0f, 0.62f, 0.0f);
}

static WeaponStateMessage makeWeaponStateMessage(const WeaponComponent& weapon)
{
    WeaponStateMessage msg;
    msg.ammo = weapon.ammo;
    msg.max_ammo = weapon.max_ammo;
    msg.reserve_ammo = weapon.reserve_ammo;
    msg.max_reserve_ammo = weapon.max_reserve_ammo;
    msg.weapon_type = static_cast<uint8_t>(weapon.weapon_type);
    msg.reloading = weapon.reloading ? 1 : 0;
    msg.fire_cooldown = weapon.fire_cooldown;
    msg.reload_timer = weapon.reload_timer;
    msg.accuracy_penalty = weapon.accuracy_penalty;
    msg.recoil_index = weapon.recoil_index;
    msg.spread = WeaponSystem::computeSpread(weapon, {});
    return msg;
}

static WeaponSystem::AccuracyContext getWeaponAccuracyContext(world& w, entt::entity player_entity)
{
    WeaponSystem::AccuracyContext context;
    if (!w.registry.valid(player_entity))
        return context;

    if (w.registry.all_of<RigidBodyComponent>(player_entity))
    {
        const glm::vec3 velocity = w.registry.get<RigidBodyComponent>(player_entity).velocity;
        context.horizontal_speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    }
    if (w.registry.all_of<PlayerComponent>(player_entity))
    {
        const auto& player = w.registry.get<PlayerComponent>(player_entity);
        context.grounded = player.grounded;
        context.max_speed = player.speed;
    }
    if (w.registry.all_of<CharacterControllerComponent>(player_entity))
    {
        const auto& controller = w.registry.get<CharacterControllerComponent>(player_entity);
        context.grounded = controller.grounded;
        context.max_speed = controller.move_speed;
    }
    return context;
}

static WeaponStateMessage makeWeaponStateMessage(world& w, entt::entity player_entity, const WeaponComponent& weapon)
{
    WeaponStateMessage msg = makeWeaponStateMessage(weapon);
    msg.spread = WeaponSystem::computeSpread(weapon, getWeaponAccuracyContext(w, player_entity));
    return msg;
}

static void sendWeaponState(uint16_t client_id, entt::entity player_entity, const WeaponComponent& weapon)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    const WeaponStateMessage msg = (w && w->registry.valid(player_entity))
        ? makeWeaponStateMessage(*w, player_entity, weapon)
        : makeWeaponStateMessage(weapon);

    BitWriter writer;
    CombatSerializer::serialize(writer, msg);
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
    const WeaponComponent& weapon,
    float shot_spread)
{
    world* w = g_server_services->game_world;

    const auto& shooter_transform = w->registry.get<TransformComponent>(shooter_entity);
    const auto& weapon_def = getWeaponDef(weapon.weapon_type);
    const glm::vec3 ray_origin = getShootOrigin(shooter_transform);
    const glm::vec3 ray_dir = makeAimDirection(input.camera_pitch, input.camera_yaw);

    const uint32_t rewind_tick = getLagCompensatedAimTick(acknowledged_server_tick);

    // Per-pellet hits: every pellet that hits a player applies damage
    // independently (fixes best-pellet-only shotgun bug).
    uint32_t best_hit_entity_id = 0;
    uint16_t best_hit_client_id = 0;
    glm::vec3 best_hit_point = ray_origin + ray_dir * weapon_def.range;
    float best_hit_dist = weapon_def.range;
    entt::entity best_hit_ent = entt::null;
    std::unordered_map<entt::entity, int> pellet_hits;
    std::unordered_map<entt::entity, glm::vec3> pellet_hit_points;
    auto player_view = w->registry.view<NetworkedEntity, TransformComponent, FpsHealthComponent>();
    for (int pellet = 0; pellet < weapon_def.pellets; pellet++) {
        glm::vec3 pellet_dir = WeaponSystem::applySpread(ray_dir, shot_spread,
            input.tick * 31 + pellet * 7919 + shooter_client_id);
        const PhysicsSystem::RaycastResult trace_hit =
            w->raycastClosest(ray_origin, pellet_dir, weapon_def.range, shooter_entity);
        const bool trace_hit_player = trace_hit.hit && w->registry.valid(trace_hit.entity) &&
            w->registry.all_of<FpsHealthComponent>(trace_hit.entity);
        if (trace_hit.hit && !trace_hit_player && trace_hit.distance < best_hit_dist) {
            best_hit_dist = trace_hit.distance; best_hit_point = trace_hit.hit_point;
            best_hit_entity_id = 0; best_hit_client_id = 0; best_hit_ent = entt::null;
        }
        for (auto entity : player_view) {
            if (entity == shooter_entity) continue;
            auto& net = player_view.get<NetworkedEntity>(entity);
            auto& health = player_view.get<FpsHealthComponent>(entity);
            auto& trans = player_view.get<TransformComponent>(entity);
            if (!health.alive) continue;
            glm::vec3 target_position = trans.position;
            ComponentSnapshot rewind_snapshot;
            if (g_server_network.sampleLagCompensatedEntity(net.network_id, rewind_tick, rewind_snapshot))
                target_position = rewind_snapshot.position;
            glm::vec3 hit_point;
            if (rayCapsuleIntersect(ray_origin, pellet_dir, target_position, 0.4f, 1.8f, hit_point)) {
                float dist = glm::distance(ray_origin, hit_point);
                if (trace_hit.hit && trace_hit.entity != entity && trace_hit.distance + 0.02f < dist) continue;
                if (dist <= weapon_def.range) {
                    pellet_hits[entity]++;
                    pellet_hit_points[entity] = hit_point;
                    if (dist < best_hit_dist) {
                        best_hit_dist = dist; best_hit_point = hit_point;
                        best_hit_entity_id = net.network_id; best_hit_client_id = net.owner_client_id; best_hit_ent = entity;
                    }
                }
            }
        }
    }
    for (auto& kv : pellet_hits) {
        entt::entity victim_ent = kv.first;
        if (!w->registry.valid(victim_ent)) continue;
        auto& victim_health = w->registry.get<FpsHealthComponent>(victim_ent);
        if (!victim_health.alive) continue;
        uint16_t victim_client = w->registry.get<NetworkedEntity>(victim_ent).owner_client_id;
        glm::vec3 hp = pellet_hit_points[victim_ent];
        int32_t total_damage = weapon_def.damage * kv.second;
        victim_health.takeDamage(total_damage);
        DamageEventMessage dmg_msg;
        dmg_msg.attacker_client_id = shooter_client_id; dmg_msg.victim_client_id = victim_client;
        dmg_msg.damage = total_damage; dmg_msg.health_remaining = victim_health.health; dmg_msg.hit_position = hp;
        BitWriter dmg_writer; CombatSerializer::serialize(dmg_writer, dmg_msg);
        g_server_network.sendReliableToClient(victim_client, dmg_writer);
        if (shooter_client_id != victim_client) g_server_network.sendReliableToClient(shooter_client_id, dmg_writer);
        LOG_ENGINE_TRACE("Player {} hit player {} for {} damage ({} pellets, hp: {})", shooter_client_id, victim_client, total_damage, kv.second, victim_health.health);
        if (!victim_health.alive) {
            auto& victim_trans = w->registry.get<TransformComponent>(victim_ent);
            // Death drop: primary weapon becomes a pickup.
            if (w->registry.all_of<InventoryComponent, PlayerLoadoutComponent, WeaponComponent>(victim_ent)) {
                auto& vinv = w->registry.get<InventoryComponent>(victim_ent);
                auto& vlo = w->registry.get<PlayerLoadoutComponent>(victim_ent);
                auto& vw = w->registry.get<WeaponComponent>(victim_ent);
                int prev = vlo.active_slot; vlo.active_slot = (int)WeaponSlot::Primary;
                if (auto dropped = FpsLogic::dropActive(vinv, vlo, vw)) {
                    entt::entity de = w->registry.create();
                    TransformComponent dt; dt.position = victim_trans.position; w->registry.emplace<TransformComponent>(de, dt);
                    w->registry.emplace<DroppedWeaponComponent>(de, DroppedWeaponComponent{dropped->type, dropped->ammo, dropped->reserve, victim_client, 30.0f});
                }
                vlo.active_slot = prev;
            }
            g_game_rules.onPlayerKilled(*w, shooter_client_id, victim_client);
            if (g_competitive && shooter_client_id != victim_client) {
                entt::entity killer_ent = findPlayerEntity(*w, shooter_client_id);
                if (killer_ent != entt::null && w->registry.all_of<Competitive::MoneyComponent>(killer_ent)) {
                    const WeaponCatalogDef* def = findCatalogByWeaponType(weapon.weapon_type);
                    // Legacy weapons have no catalog entry: default kill award data.
                    if (def) g_economy.awardKill(w->registry.get<Competitive::MoneyComponent>(killer_ent), def);
                    else w->registry.get<Competitive::MoneyComponent>(killer_ent).wallet.add(300);
                    sendEconomyState(shooter_client_id, killer_ent);
                }
            }
            PlayerDiedMessage death_msg;
            death_msg.victim_client_id = victim_client; death_msg.killer_client_id = shooter_client_id; death_msg.death_position = victim_trans.position;
            BitWriter death_writer; CombatSerializer::serialize(death_writer, death_msg);
            for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
                const ClientInfo* client = g_server_network.getClientInfo(i);
                if (client && client->peer) g_server_network.sendReliableToClient(i, death_writer);
            }
            LOG_ENGINE_INFO("Player {} killed by player {}", victim_client, shooter_client_id);
            // Competitive Live: no respawn (spectates until next Freeze).
            // Warmup keeps DM-style respawn.
            if (!g_competitive || g_round_system.state.phase == Competitive::RoundPhase::Warmup)
                g_game_rules.queueRespawn(victim_client);
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


static WeaponType primaryForClient(uint16_t client_id, uint8_t team_id)
{
    // Defenders (and even DM clients) get the shotgun primary so it is reachable.
    if (team_id == FpsLogic::kTeamDefenders) return WeaponType::SHOTGUN;
    if (g_game_rules.match.config.mode == FpsLogic::FpsGameMode::Deathmatch && (client_id % 2 == 0)) return WeaponType::SHOTGUN;
    return WeaponType::RIFLE;
}
static void spawnDroppedWeapon(world& w, const glm::vec3& pos, const FpsLogic::DroppedItem& item, uint16_t by)
{
    entt::entity de = w.registry.create();
    TransformComponent dt; dt.position = pos;
    w.registry.emplace<TransformComponent>(de, dt);
    w.registry.emplace<DroppedWeaponComponent>(de, DroppedWeaponComponent{item.type, item.ammo, item.reserve, by, 30.0f});
}
static bool doDropWeapon(uint16_t client_id, entt::entity player_entity)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.valid(player_entity)) return false;
    if (!w->registry.all_of<InventoryComponent, PlayerLoadoutComponent, WeaponComponent, TransformComponent>(player_entity)) return false;
    auto& inv = w->registry.get<InventoryComponent>(player_entity);
    auto& lo = w->registry.get<PlayerLoadoutComponent>(player_entity);
    auto& weapon = w->registry.get<WeaponComponent>(player_entity);
    auto dropped = FpsLogic::dropActive(inv, lo, weapon);
    if (!dropped) return false;
    glm::vec3 pos = w->registry.get<TransformComponent>(player_entity).position + glm::vec3(0.0f, 0.3f, 0.0f);
    spawnDroppedWeapon(*w, pos, *dropped, client_id);
    sendWeaponState(client_id, player_entity, weapon);
    LOG_ENGINE_INFO("Player {} dropped weapon {}", client_id, (int)dropped->type);
    return true;
}
static bool doWeaponSelect(uint16_t client_id, entt::entity player_entity, uint8_t sel)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.valid(player_entity)) return false;
    if (!w->registry.all_of<InventoryComponent, PlayerLoadoutComponent, WeaponComponent>(player_entity)) return false;
    auto& inv = w->registry.get<InventoryComponent>(player_entity);
    auto& lo = w->registry.get<PlayerLoadoutComponent>(player_entity);
    auto& weapon = w->registry.get<WeaponComponent>(player_entity);
    if (!FpsLogic::switchByWeaponSelect(inv, lo, weapon, sel)) return false;
    sendWeaponState(client_id, player_entity, weapon);
    LOG_ENGINE_INFO("Player {} switched to weapon {} (slot {})", client_id, (int)weapon.weapon_type, lo.active_slot);
    return true;
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

    if (!w->registry.all_of<TransformComponent, FpsHealthComponent, WeaponComponent>(player_entity)) {
        return;
    }

    auto& health = w->registry.get<FpsHealthComponent>(player_entity);
    if (!health.alive) {
        return;
    }

    auto& weapon = w->registry.get<WeaponComponent>(player_entity);
    const bool wants_attack = (input.buttons & InputFlags::ATTACK) != 0;
    const bool wants_reload = (input.buttons & InputFlags::RELOAD) != 0;
    const bool wants_drop = (input.buttons & InputFlags::DROP) != 0;
    if (wants_drop) {
        doDropWeapon(client_id, player_entity);
        return;
    }
    if (!wants_attack && !wants_reload) {
        return;
    }

    if (wants_attack) {
        const float shot_spread = WeaponSystem::computeSpread(
            weapon, getWeaponAccuracyContext(*w, player_entity));
        if (WeaponSystem::tryFire(weapon)) {
            fireServerShot(client_id, player_entity, input, acknowledged_server_tick, weapon, shot_spread);
        }
    } else if (wants_reload) {
        (void)WeaponSystem::tryReload(weapon);
    }
    sendWeaponState(client_id, player_entity, weapon);
}

// ---- Respawn handler ----

static void respawnPlayer(uint16_t client_id, const glm::vec3& spawn_pos)
{
    world* w = g_server_services->game_world;

    auto view = w->registry.view<NetworkedEntity, TransformComponent, FpsHealthComponent>();
    for (auto entity : view) {
        auto& net = view.get<NetworkedEntity>(entity);
        if (net.owner_client_id == client_id && net.is_player) {
            auto& trans = view.get<TransformComponent>(entity);
            auto& health = view.get<FpsHealthComponent>(entity);

            // Reset state
            health.reset();
            trans.position = spawn_pos;

            if (w->registry.all_of<RigidBodyComponent>(entity)) {
                auto& rb = w->registry.get<RigidBodyComponent>(entity);
                rb.velocity = glm::vec3(0.0f);
                rb.force = glm::vec3(0.0f);
            }

            ensurePlayerPhysicsBody(*w, entity);

            if (w->registry.all_of<WeaponComponent, InventoryComponent, PlayerLoadoutComponent>(entity)) {
                auto& weapon = w->registry.get<WeaponComponent>(entity);
                auto& inv2 = w->registry.get<InventoryComponent>(entity);
                auto& lo2 = w->registry.get<PlayerLoadoutComponent>(entity);
                uint8_t team2 = w->registry.all_of<TeamComponent>(entity) ? w->registry.get<TeamComponent>(entity).team_id : FpsLogic::kTeamNone;
                FpsLogic::grantLoadout(inv2, lo2, weapon, primaryForClient(client_id, team2));
                sendWeaponState(client_id, entity, weapon);
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



// Equivalence (bots.md S7): humans reach movement in ServerNetworkManager::handleInputCommand
// (simulate_character_controller / SharedMovement::simulate, then input_sample_handler).
// Bots have no ENet client, so consumeBotInputSample performs the identical steps in the
// identical order with the synthesized sample, then calls the same handler body
// (bomb USE bookkeeping + processWeaponInput + weapon-select edge).
static void consumeBotInputSample(uint16_t client_id, entt::entity e, const InputSample& input, float dt)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.valid(e)) return;
    if (!w->registry.all_of<PlayerComponent, TransformComponent, RigidBodyComponent, FpsHealthComponent>(e)) return;
    if (g_round_system.isMovementLocked()) return; // same as server input_filter (Freeze)
    if (!w->registry.get<FpsHealthComponent>(e).alive) return; // same as server input_filter
    auto& player = w->registry.get<PlayerComponent>(e);
    auto& transform = w->registry.get<TransformComponent>(e);
    auto& rigidbody = w->registry.get<RigidBodyComponent>(e);
    transform.rotation.y = glm::degrees(input.camera_yaw);
    transform.rotation.x = glm::degrees(input.camera_pitch);
    Net::MovementInput mi; mi.move_forward = input.move_forward; mi.move_right = input.move_right;
    mi.camera_yaw = input.camera_yaw; mi.camera_pitch = input.camera_pitch; mi.buttons = input.buttons;
    Net::MovementConfig mc; mc.speed = player.speed; mc.jump_force = player.jump_force;
    mc.fixed_delta = w->fixed_delta > 0.0f ? w->fixed_delta : dt;
    Net::MovementState result;
    if (w->registry.all_of<CharacterControllerComponent>(e)) {
        CharacterMoveInput ci; ci.move_forward = mi.move_forward; ci.move_right = mi.move_right; ci.camera_yaw = mi.camera_yaw; ci.camera_pitch = mi.camera_pitch;
        if ((mi.buttons & InputFlags::JUMP) != 0) ci.buttons |= CharacterMoveFlags::Jump;
        if ((mi.buttons & InputFlags::CROUCH) != 0) ci.buttons |= CharacterMoveFlags::Crouch;
        if ((mi.buttons & InputFlags::SPRINT) != 0) ci.buttons |= CharacterMoveFlags::Sprint;
        auto cs = w->simulate_character_controller(e, ci, mc.fixed_delta);
        result.position = cs.position; result.velocity = cs.velocity; result.grounded = cs.grounded; result.ground_normal = cs.ground_normal;
    } else {
        Net::MovementState cur; cur.position = transform.position; cur.velocity = rigidbody.velocity; cur.grounded = player.grounded; cur.ground_normal = player.ground_normal;
        result = Net::SharedMovement::simulate(mi, cur, mc);
    }
    transform.position = result.position; rigidbody.velocity = result.velocity; player.grounded = result.grounded; player.ground_normal = result.ground_normal;
    g_bomb_use_held[client_id] = (input.buttons & InputFlags::USE) != 0;
    g_bomb_last_buttons[client_id] = input.buttons;
    processWeaponInput(client_id, e, input, g_server_network.getCurrentTick());
    uint8_t sel = Net::getWeaponSelect(input.buttons);
    auto it = g_bot_last_weapon_select.find(client_id);
    if (sel != 0 && (it == g_bot_last_weapon_select.end() || it->second != sel)) doWeaponSelect(client_id, e, sel);
    g_bot_last_weapon_select[client_id] = sel;
}

static bool loadBotNavMesh()
{
    if (g_bots_attempted) return g_bots_enabled;
    g_bots_attempted = true;
    static const char* kNavCandidates[] = {"assets/levels/main.nav", "Templates/FPSShooter/assets/levels/main.nav", "../Templates/FPSShooter/assets/levels/main.nav"};
    for (const char* path : kNavCandidates) {
        Navigation::NavMesh mesh;
        if (Navigation::NavMeshSerializer::load(mesh, path) && mesh.valid) {
            g_bot_nav.mesh = std::move(mesh); g_bot_nav.loaded = true; g_bots_enabled = true;
            LOG_ENGINE_INFO("Bots: navmesh loaded from {}", path);
            return true;
        }
    }
    g_bots_enabled = false; // F-AI-01: never straight-line wall walking
    if (!g_bots_error_logged) { g_bots_error_logged = true; LOG_ENGINE_ERROR("Bots disabled: navmesh missing/invalid (F-AI-01); match continues human-only. Fix: run server once with FPS_BAKE_NAV=1 FPS_GAME_MODE=competitive to bake assets/levels/main.nav, then relaunch with FPS_BOTS=N (see Templates/FPSShooter/BOTS.txt)"); }
    return false;
}

// Explicit opt-in bake (FPS_BAKE_NAV=1): generate from the loaded level registry,
// save to the first loadBotNavMesh candidate whose directory exists, verify by
// round-trip load. Never writes on generation failure. Runtime (no env) unchanged.
static bool bakeBotNavMeshIfRequested()
{
    const char* env = std::getenv("FPS_BAKE_NAV");
    if (!env || std::atoi(env) == 0) return false;
    static bool s_done = false;
    if (s_done) return g_bot_nav.loaded;
    s_done = true;
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) { LOG_ENGINE_ERROR("BakeNav: no game world; cannot bake"); return false; }
    static const char* kNavCandidates[] = {"assets/levels/main.nav", "Templates/FPSShooter/assets/levels/main.nav", "../Templates/FPSShooter/assets/levels/main.nav"};
    std::string out_path = kNavCandidates[0];
    for (const char* cand : kNavCandidates) {
        std::filesystem::path p(cand);
        if (p.has_parent_path() && std::filesystem::is_directory(p.parent_path())) { out_path = cand; break; }
    }
    Navigation::NavMeshConfig cfg;
    cfg.agent_height = 1.8f; cfg.agent_radius = 0.35f; cfg.cell_size = 0.25f;
    cfg.cell_height = 0.2f; cfg.max_climb = 0.5f; cfg.max_slope_angle = 45.0f;
    LOG_ENGINE_INFO("BakeNav: generating navmesh (agent h={} r={} cell={} climb={}) -> {}", cfg.agent_height, cfg.agent_radius, cfg.cell_size, cfg.max_climb, out_path);
    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh baked = Navigation::NavMeshGenerator::generate(w->registry, cfg, &stats);
    if (!baked.valid || baked.total_polys <= 0) {
        LOG_ENGINE_ERROR("BakeNav: generation produced 0 polys (source_tris={}); NOT writing {}", stats.source_triangles, out_path);
        return false;
    }
    if (!Navigation::NavMeshSerializer::save(baked, out_path)) {
        LOG_ENGINE_ERROR("BakeNav: serializer save failed to {}", out_path);
        return false;
    }
    Navigation::NavMesh check;
    if (!Navigation::NavMeshSerializer::load(check, out_path) || !check.valid || check.total_polys <= 0) {
        LOG_ENGINE_ERROR("BakeNav: round-trip load failed for {}", out_path);
        return false;
    }
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(out_path, ec);
    LOG_ENGINE_INFO("BakeNav: wrote {} polys={} bake_ms={:.1f} source_tris={} roundtrip_polys={} bytes={}",
        out_path, baked.total_polys, stats.time_ms, stats.source_triangles, check.total_polys, ec ? 0 : (long long)bytes);
    return true;
}

static void spawnBotsIfNeeded()
{
    if (!g_bots_enabled) return;
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    int32_t existing = 0;
    { auto v = w->registry.view<Bots::BotComponent>(); for (auto e : v) { (void)e; ++existing; } }
    for (int32_t i = existing; i < g_bot_target_count; ++i) {
        // Fill empty team slots only (team_size per team).
        int32_t na = 0, nd = 0;
        auto tv = w->registry.view<NetworkedEntity, TeamComponent>();
        for (auto e : tv) { if (!tv.get<NetworkedEntity>(e).is_player) continue; uint8_t tid = tv.get<TeamComponent>(e).team_id; if (tid == FpsLogic::kTeamAttackers) ++na; else if (tid == FpsLogic::kTeamDefenders) ++nd; }
        if (na >= g_round_system.config.team_size && nd >= g_round_system.config.team_size) break;
        uint16_t bot_client = (uint16_t)(Bots::kBotClientIdBase + (uint32_t)i);
        spawnPlayerForClient(bot_client); // same player-spawn path as humans
        entt::entity e = findPlayerEntity(*w, bot_client);
        if (e == entt::null) { LOG_ENGINE_ERROR("Bots: spawn failed for bot client {}", bot_client); break; }
        auto& bot = w->registry.emplace<Bots::BotComponent>(e);
        bot.difficulty = botDifficultyFromEnv(); bot.pawn = e; bot.bot_index = i;
        bot.rng_state = 0x9E3779B9u ^ ((uint32_t)bot_client * 2654435761u); if (!bot.rng_state) bot.rng_state = 1;
        w->registry.emplace<Bots::BotPerceptComponent>(e);
        w->registry.emplace<Bots::BotIntentComponent>(e);
        w->registry.emplace<Bots::NavAgentComponent>(e);
        w->registry.emplace<Bots::SteeringComponent>(e);
        LOG_ENGINE_INFO("Bots: spawned bot client {} difficulty {}", bot_client, (int)bot.difficulty);
    }
}

static void executeBotBuy(uint16_t client_id, entt::entity e)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.all_of<TeamComponent, Competitive::MoneyComponent, InventoryComponent, PlayerLoadoutComponent, WeaponComponent, FpsHealthComponent, TransformComponent, Bots::BotComponent>(e)) return;
    auto& bot = w->registry.get<Bots::BotComponent>(e);
    if (bot.buy_done) return;
    bot.buy_done = true; // one plan per Freeze; rejected items are skipped, never retried
    uint8_t team = w->registry.get<TeamComponent>(e).team_id;
    auto plan = Bots::selectBuyPlan(w->registry.get<Competitive::MoneyComponent>(e).money(), Competitive::teamFromId(team), bot.difficulty, w->registry.get<Competitive::MoneyComponent>(e).loss_streak);
    std::vector<glm::vec3> spawns;
    for (auto& sp : g_game_rules.getTeamSpawnPoints()) if (sp.team_id == team) spawns.push_back(sp.position);
    glm::vec3 pos = w->registry.get<TransformComponent>(e).position;
    for (const char* alias : plan.aliases) {
        const WeaponCatalogDef* def = findCatalogByAlias(alias); if (!def) continue;
        Competitive::BuyContext ctx; ctx.client_id = client_id; ctx.request_seq = ++g_bot_buy_seq[client_id];
        ctx.phase = g_round_system.state.phase; ctx.seconds_into_live = 0.0f;
        ctx.alive = w->registry.get<FpsHealthComponent>(e).alive;
        ctx.in_buy_zone = Competitive::isInBuyZone(pos, spawns, g_round_system.config.buy_zone_radius);
        ctx.team_id = team; ctx.money = &w->registry.get<Competitive::MoneyComponent>(e);
        ctx.inventory = &w->registry.get<InventoryComponent>(e); ctx.loadout = &w->registry.get<PlayerLoadoutComponent>(e);
        ctx.weapon = &w->registry.get<WeaponComponent>(e); ctx.armor = w->registry.all_of<ArmorComponent>(e) ? &w->registry.get<ArmorComponent>(e) : nullptr;
        FpsLogic::DroppedItem dropped{WeaponType::COUNT, 0, 0};
        Competitive::BuyResult r = g_buy_system.buy(ctx, def->id, &dropped); // same BuySystem server path as human BUY_REQUEST
        if (r != Competitive::BuyResult::Ok) continue; // skip-on-reject
        if (dropped.type != WeaponType::COUNT) spawnDroppedWeapon(*w, pos + glm::vec3(0.0f, 0.3f, 0.0f), dropped, client_id);
        if (def->kind != BuyItemKind::Equipment) sendWeaponState(client_id, e, *ctx.weapon);
    }
    sendEconomyState(client_id, e);
}

static void tickBots(float dt)
{
    if (!g_bots_enabled) return;
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || dt <= 0.0f) return;
    g_bot_world_time += dt;
    const bool bot_debug_dump_now = aiDebugLevel() >= 1 && (g_bot_world_time - g_bot_debug_last_dump >= 1.0f);
    if (bot_debug_dump_now) g_bot_debug_last_dump = g_bot_world_time;
    spawnBotsIfNeeded();
    std::vector<entt::entity> bots;
    { auto v = w->registry.view<Bots::BotComponent>(); for (auto e : v) bots.push_back(e); }
    for (entt::entity e : bots) {
        if (!w->registry.valid(e) || !w->registry.all_of<TransformComponent, FpsHealthComponent, TeamComponent, NetworkedEntity>(e)) continue;
        auto& bot = w->registry.get<Bots::BotComponent>(e);
        auto& percept = w->registry.get<Bots::BotPerceptComponent>(e);
        auto& intent = w->registry.get<Bots::BotIntentComponent>(e);
        auto& nav = w->registry.get<Bots::NavAgentComponent>(e);
        auto& steer = w->registry.get<Bots::SteeringComponent>(e);
        uint16_t client_id = w->registry.get<NetworkedEntity>(e).owner_client_id;
        glm::vec3 pos = w->registry.get<TransformComponent>(e).position;
        bool alive = w->registry.get<FpsHealthComponent>(e).alive;
        uint8_t team_id = w->registry.get<TeamComponent>(e).team_id;
        // S1 perception (10 Hz staggered by bot_index % 6 fixed ticks)
        if (((uint32_t)(g_bot_world_time * 60.0f) + (uint32_t)bot.bot_index) % 6u == 0u) {
            std::vector<Bots::EnemyCandidate> enemies;
            auto ev = w->registry.view<NetworkedEntity, TeamComponent, TransformComponent, FpsHealthComponent>();
            for (auto en : ev) { if (en == e || !ev.get<NetworkedEntity>(en).is_player || !ev.get<FpsHealthComponent>(en).alive) continue; if (ev.get<TeamComponent>(en).team_id == team_id) continue; enemies.push_back(Bots::EnemyCandidate{en, ev.get<TransformComponent>(en).position}); }
            glm::vec3 eye = pos + glm::vec3(0.0f, 1.62f, 0.0f);
            Bots::updatePerception(percept, eye, bot.aim_yaw, enemies, bot.difficulty, 0.1f,
                [&](const glm::vec3& origin, const glm::vec3& point) { glm::vec3 d = point - origin; float dist = glm::length(d); if (dist < 0.01f) return false; auto hit = w->raycastClosest(origin, d / dist, dist, e); return hit.hit && hit.distance + 0.02f < dist; });
        } else { percept.last_seen_age += dt; percept.heard_age += dt; }
        // S2 decision (consumes percept + match summary only)
        Bots::BotDecisionInput din; din.phase = g_round_system.state.phase; din.team = Competitive::teamFromId(team_id);
        din.alive = alive; din.is_carrier = g_bomb_system.bomb.carrier == e; din.bomb_planted = g_bomb_system.isPlanted();
        din.bot_pos = pos; din.health = w->registry.get<FpsHealthComponent>(e).health; din.dt = dt;
        if (!g_bomb_system.config.sites.empty()) { const auto& s = g_bomb_system.config.sites[(size_t)std::abs(bot.bot_index) % g_bomb_system.config.sites.size()]; din.site_pos = s.centre; din.has_site = true; uint8_t si = 0; din.at_site = g_bomb_system.config.inSite(pos, si); }
        if (din.bomb_planted) { din.bomb_pos = g_bomb_system.bomb.planted_position; din.has_bomb_pos = true; din.at_bomb = glm::distance(pos, din.bomb_pos) <= g_bomb_system.config.defuse_range; }
        const int32_t dbg = aiDebugLevel();
        const uint64_t trans_before = g_bot_transitions.total_pushed;
        Bots::decide(bot, percept, intent, din, dbg >= 1 ? &g_bot_transitions : nullptr, g_bot_input_tick);
        if (dbg >= 2 && g_bot_transitions.total_pushed > trans_before) { uint64_t n = std::min<uint64_t>(g_bot_transitions.total_pushed - trans_before, g_bot_transitions.size()); for (uint64_t i = 0; i < n; ++i) LOG_ENGINE_INFO("{}", Bots::formatTransition(g_bot_transitions.at(g_bot_transitions.size() - (size_t)n + (size_t)i))); }
        if (bot.state == Bots::BotState::Buy) executeBotBuy(client_id, e);
        if (g_round_system.state.phase == Competitive::RoundPhase::Live) bot.buy_done = false;
        // S3 path request (only when a navmesh path can be produced; no wall walking)
        nav.repath_timer -= dt;
        if (intent.has_move_goal && g_bot_nav.loaded) {
            glm::vec3 goal = intent.move_goal; glm::vec3 to_goal = goal - nav.goal; to_goal.y = 0;
            if ((!nav.has_goal || glm::length(to_goal) > 2.0f) && nav.repath_timer <= 0.0f) {
                nav.repath_timer = nav.repath_cooldown;
                Navigation::NavPath path = Navigation::NavMeshPathfinder::findPath(g_bot_nav.mesh, pos, goal);
                if (path.valid) Bots::setPath(nav, path.waypoints, goal); else { nav.path_valid = false; nav.has_goal = false; ++nav.path_failures; }
            }
        } else if (intent.has_move_goal) { nav.path_valid = false; nav.has_goal = false; }
        // No move goal (incl. Engage): drop any stale path so steering does not
        // walk the bot toward its pre-Engage goal while it aims/strafes.
        if (!intent.has_move_goal) { nav.has_goal = false; nav.desired_velocity = glm::vec3(0); }
        // S4 steering + S5 synthesis into the same consumption path as humans
        Bots::updateSteering(nav, steer, pos, dt);
        Net::InputSample sample = aiPauseBots() ? Bots::synthesizeNeutralInput(++g_bot_input_tick, bot.aim_yaw, bot.aim_pitch)
                                                : Bots::synthesizeInput(bot, nav, intent, ++g_bot_input_tick, dt);
        consumeBotInputSample(client_id, e, sample, dt);
        if (dbg >= 1) {
            if (g_bot_stuck_latches[bot.bot_index].check(nav.stuck)) LOG_ENGINE_WARN("[BotDebug] bot_stuck bot={} (F-AI-03)", bot.bot_index);
            if (bot_debug_dump_now) { Bots::BotDumpInput dump; dump.bot_index = bot.bot_index; dump.team = (int32_t)team_id; dump.state = bot.state; dump.hp = w->registry.get<FpsHealthComponent>(e).health; dump.pos = pos; dump.goal = nav.goal; dump.has_goal = nav.has_goal; dump.path_valid = nav.path_valid; dump.stuck = nav.stuck; dump.visible_age = percept.last_seen_age; dump.heard_age = percept.heard_age; LOG_ENGINE_INFO("{}", Bots::formatBotDump(dump)); }
        }
    }
}

// ---- Competitive mode helpers ----
static entt::entity findPlayerEntity(world& w, uint16_t client_id)
{
    auto view = w.registry.view<NetworkedEntity>();
    for (auto e : view) {
        auto& net = view.get<NetworkedEntity>(e);
        if (net.is_player && net.owner_client_id == client_id) return e;
    }
    return entt::null;
}
static void sendEconomyState(uint16_t client_id, entt::entity e)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || !w->registry.valid(e) || !w->registry.all_of<Competitive::MoneyComponent>(e)) return;
    auto& money = w->registry.get<Competitive::MoneyComponent>(e);
    EconomyStateMessage msg; msg.balance = money.money(); msg.loss_streak = money.loss_streak;
    BitWriter writer; CompetitiveSerializer::serialize(writer, msg);
    g_server_network.sendReliableToClient(client_id, writer);
}
static void broadcastRoundState()
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (w && w->registry.valid(g_match_entity)) {
        w->registry.get_or_emplace<Competitive::MatchStateComponent>(g_match_entity) = g_round_system.state;
        w->registry.get_or_emplace<CountdownTimerComponent>(g_match_entity) = g_round_system.timer;
    }
    RoundStateMessage msg;
    msg.phase = static_cast<uint8_t>(g_round_system.state.phase);
    msg.phase_time_remaining = g_round_system.state.phase_time_remaining;
    msg.phase_deadline_tick = g_round_system.state.phase_deadline_tick;
    msg.server_tick = g_round_system.state.server_tick;
    msg.round_number = g_round_system.state.round_number;
    msg.wins_attackers = g_round_system.state.wins_attackers;
    msg.wins_defenders = g_round_system.state.wins_defenders;
    msg.bomb_planted = g_round_system.state.bomb_planted ? 1 : 0;
    msg.last_reason = static_cast<uint8_t>(g_round_system.state.last_reason);
    msg.last_winner = static_cast<uint8_t>(g_round_system.state.last_winner);
    BitWriter writer; CompetitiveSerializer::serialize(writer, msg);
    for (uint16_t i = 1; i < g_server_network.getNextClientId(); i++) {
        const ClientInfo* c = g_server_network.getClientInfo(i);
        if (c && c->peer) g_server_network.sendReliableToClient(i, writer);
    }
}
static void competitiveFreezeRespawnAll()
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    std::vector<uint16_t> clients;
    auto view = w->registry.view<NetworkedEntity, TeamComponent>();
    for (auto e : view) {
        auto& net = view.get<NetworkedEntity>(e);
        if (net.is_player) clients.push_back(net.owner_client_id);
    }
    for (uint16_t cid : clients) {
        entt::entity e = findPlayerEntity(*w, cid);
        if (e == entt::null || !w->registry.all_of<TeamComponent>(e)) continue;
        uint8_t team = w->registry.get<TeamComponent>(e).team_id;
        if (team != FpsLogic::kTeamAttackers && team != FpsLogic::kTeamDefenders) continue;
        bool was_dead = w->registry.all_of<FpsHealthComponent>(e) && !w->registry.get<FpsHealthComponent>(e).alive;
        glm::vec3 pos = g_game_rules.getSpawnForPlayer(*w, cid, team);
        respawnPlayer(cid, pos); // resets health/position; loadout re-granted inside
        (void)was_dead;
        sendEconomyState(cid, e);
    }
}
// Bomb sites: no bomb-site volumes are exposed by the level, so use the
// documented BombConfig fallback - Defender spawn centres + fallback radius.
static bool loadAuthoredBombSites(std::vector<Competitive::BombSite>& out)
{
    static bool s_tried = false; static std::vector<Competitive::BombSite> s_sites; static bool s_ok = false;
    if (s_tried) { out = s_sites; return s_ok; }
    s_tried = true;
    static const char* kCandidates[] = {"assets/levels/main.sites.json", "Templates/FPSShooter/assets/levels/main.sites.json", "../Templates/FPSShooter/assets/levels/main.sites.json"};
    for (const char* path : kCandidates) {
        FILE* f = std::fopen(path, "rb"); if (!f) continue;
        std::string text; char buf[4096]; size_t n = 0; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        std::string err;
        if (!Competitive::parseAuthoredBombSites(text, s_sites, &err)) { LOG_ENGINE_ERROR("Bomb: authored sites {} rejected ({}); using fallback", path, err); s_sites.clear(); return false; }
        if (g_bot_nav.loaded) {
            for (const auto& s : s_sites) { glm::vec3 near_pt(0); if (!Navigation::NavMeshPathfinder::findNearestPoly(g_bot_nav.mesh, s.centre, near_pt)) { LOG_ENGINE_ERROR("Bomb: authored site {} off navmesh; using fallback", (int)s.site_index); s_sites.clear(); return false; } }
        }
        s_ok = true; out = s_sites;
        LOG_ENGINE_INFO("Bomb: authored sites loaded from {} count={}", path, (int)s_sites.size());
        return true;
    }
    LOG_ENGINE_INFO("Bomb: no authored sites file found; using fallback");
    return false;
}
static void configureBombSitesFallback();
static bool configureBombSites()
{
    std::vector<Competitive::BombSite> authored;
    if (loadAuthoredBombSites(authored) && authored.size() >= 2) { g_bomb_system.config.sites = authored; return true; }
    configureBombSitesFallback();
    return false;
}
static void configureBombSitesFallback()
{
    g_bomb_system.config.sites.clear();
    uint8_t idx = 0;
    for (auto& sp : g_game_rules.getTeamSpawnPoints()) {
        if (sp.team_id != FpsLogic::kTeamDefenders || idx >= 2) continue;
        g_bomb_system.config.sites.push_back(Competitive::BombSite{sp.position, g_bomb_system.config.fallback_site_radius, idx++});
    }
    if (g_bomb_system.config.sites.empty())
        g_bomb_system.config.sites.push_back(Competitive::BombSite{glm::vec3(0, 5, 0), g_bomb_system.config.fallback_site_radius, 0});
}
static void competitiveFreezeBombSetup()
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    (void)configureBombSites();
    g_bomb_system.resetForRound();
    std::vector<entt::entity> attackers;
    auto view = w->registry.view<NetworkedEntity, TeamComponent, FpsHealthComponent>();
    for (auto e : view) {
        if (!view.get<NetworkedEntity>(e).is_player) continue;
        if (view.get<TeamComponent>(e).team_id == FpsLogic::kTeamAttackers && view.get<FpsHealthComponent>(e).alive)
            attackers.push_back(e);
    }
    if (attackers.empty()) { LOG_ENGINE_INFO("Bomb: no alive Attacker to carry C4"); return; }
    // Pseudo-random carrier per round (round-seeded, deterministic per match).
    size_t pick = (static_cast<size_t>(g_round_system.state.round_number) * 2654435761u >> 8) % attackers.size();
    entt::entity carrier = attackers[pick];
    g_bomb_system.assignCarrier(carrier);
    uint16_t cid = w->registry.get<NetworkedEntity>(carrier).owner_client_id;
    LOG_ENGINE_INFO("Bomb: carrier=client {} sites={}", cid, (int)g_bomb_system.config.sites.size());
}
static bool bombActorMoved(world& w, entt::entity e, uint16_t client_id)
{
    auto it = g_bomb_last_buttons.find(client_id);
    if (it != g_bomb_last_buttons.end() && (it->second & (InputFlags::MOVE_FORWARD | InputFlags::MOVE_BACK | InputFlags::MOVE_LEFT | InputFlags::MOVE_RIGHT)) != 0) return true;
    if (w.registry.all_of<RigidBodyComponent>(e)) {
        glm::vec3 v = w.registry.get<RigidBodyComponent>(e).velocity;
        if (std::sqrt(v.x * v.x + v.z * v.z) > g_bomb_system.config.move_speed_threshold) return true;
    }
    return false;
}
static bool bombActorDamaged(world& w, entt::entity e)
{
    if (!w.registry.all_of<FpsHealthComponent>(e)) return false;
    auto it = g_bomb_prev_health.find(static_cast<uint32_t>(e));
    return it != g_bomb_prev_health.end() && w.registry.get<FpsHealthComponent>(e).health < it->second;
}
static bool bombActorSwitched(world& w, entt::entity e)
{
    if (!w.registry.all_of<WeaponComponent>(e)) return false;
    auto it = g_bomb_prev_weapon.find(static_cast<uint32_t>(e));
    return it != g_bomb_prev_weapon.end() && static_cast<uint8_t>(w.registry.get<WeaponComponent>(e).weapon_type) != it->second;
}
static void tickBombServer(float dt)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w || g_round_system.state.phase != Competitive::RoundPhase::Live) return;
    using B = Competitive::BombState;
    // Dropped bomb: Attackers-only walk-over pickup.
    if (g_bomb_system.bomb.state == B::Dropped) {
        auto pv = w->registry.view<NetworkedEntity, TeamComponent, TransformComponent, FpsHealthComponent>();
        for (auto e : pv) {
            if (!pv.get<NetworkedEntity>(e).is_player || !pv.get<FpsHealthComponent>(e).alive) continue;
            if (g_bomb_system.tryPickup(e, Competitive::teamFromId(pv.get<TeamComponent>(e).team_id), pv.get<TransformComponent>(e).position)) break;
        }
    }
    Competitive::BombTickInput in;
    in.paused = g_round_system.state.paused;
    entt::entity carrier = g_bomb_system.bomb.carrier;
    if (carrier != entt::null && w->registry.valid(carrier) && w->registry.all_of<TransformComponent, FpsHealthComponent, NetworkedEntity>(carrier)) {
        glm::vec3 pos = w->registry.get<TransformComponent>(carrier).position;
        uint16_t cid = w->registry.get<NetworkedEntity>(carrier).owner_client_id;
        in.carrier_position = pos;
        in.plant_dead = !w->registry.get<FpsHealthComponent>(carrier).alive;
        auto uit = g_bomb_use_held.find(cid);
        in.plant_use = uit != g_bomb_use_held.end() && uit->second;
        in.plant_in_site = g_bomb_system.config.inSite(pos, in.plant_site);
        in.plant_moved = bombActorMoved(*w, carrier, cid);
        in.plant_damaged = bombActorDamaged(*w, carrier);
        in.plant_weapon_switched = bombActorSwitched(*w, carrier);
    } else if (carrier != entt::null && !w->registry.valid(carrier)) {
        in.plant_dead = true; // carrier entity gone (disconnect handled explicitly too)
    }
    if (g_bomb_system.bomb.state == B::Planted || g_bomb_system.bomb.state == B::Defusing) {
        // Prefer the locked defuser; otherwise nearest USE-holding Defender in range.
        entt::entity best = g_bomb_system.bomb.defuser; float best_dist = 1e30f;
        auto pv = w->registry.view<NetworkedEntity, TeamComponent, TransformComponent, FpsHealthComponent>();
        for (auto e : pv) {
            if (!pv.get<NetworkedEntity>(e).is_player || !pv.get<FpsHealthComponent>(e).alive) continue;
            if (pv.get<TeamComponent>(e).team_id != FpsLogic::kTeamDefenders) continue;
            float d = glm::distance(pv.get<TransformComponent>(e).position, g_bomb_system.bomb.planted_position);
            uint16_t cid = pv.get<NetworkedEntity>(e).owner_client_id;
            auto uit = g_bomb_use_held.find(cid);
            bool use = uit != g_bomb_use_held.end() && uit->second;
            if (e == g_bomb_system.bomb.defuser) { best = e; best_dist = d; break; }
            if (use && d <= g_bomb_system.config.defuse_range && d < best_dist) { best = e; best_dist = d; }
        }
        if (best != entt::null && w->registry.valid(best) && w->registry.all_of<TransformComponent, FpsHealthComponent, NetworkedEntity>(best)) {
            uint16_t cid = w->registry.get<NetworkedEntity>(best).owner_client_id;
            auto uit = g_bomb_use_held.find(cid);
            in.defuse_actor = best;
            in.defuse_use = uit != g_bomb_use_held.end() && uit->second;
            in.defuse_in_range = glm::distance(w->registry.get<TransformComponent>(best).position, g_bomb_system.bomb.planted_position) <= g_bomb_system.config.defuse_range;
            in.defuse_has_kit = w->registry.all_of<ArmorComponent>(best) && w->registry.get<ArmorComponent>(best).has_defuse_kit;
            in.defuse_moved = bombActorMoved(*w, best, cid);
            in.defuse_damaged = bombActorDamaged(*w, best);
            in.defuse_dead = !w->registry.get<FpsHealthComponent>(best).alive;
        }
    }
    g_bomb_system.tick(dt, in);
    // Refresh damage/weapon baselines for the next tick.
    auto all = w->registry.view<NetworkedEntity, FpsHealthComponent>();
    for (auto e : all) {
        g_bomb_prev_health[static_cast<uint32_t>(e)] = all.get<FpsHealthComponent>(e).health;
        if (w->registry.all_of<WeaponComponent>(e)) g_bomb_prev_weapon[static_cast<uint32_t>(e)] = static_cast<uint8_t>(w->registry.get<WeaponComponent>(e).weapon_type);
    }
    // Plant economy: planter +300, every Attacker +800 (planter included as team member).
    entt::entity planter = entt::null;
    if (g_bomb_system.consumePlantCompleted(planter) && planter != entt::null && w->registry.valid(planter)) {
        auto pv = w->registry.view<NetworkedEntity, TeamComponent, Competitive::MoneyComponent>();
        for (auto e : pv) {
            if (!pv.get<NetworkedEntity>(e).is_player) continue;
            if (e == planter) g_economy.awardPlantPlanter(pv.get<Competitive::MoneyComponent>(e));
            if (pv.get<TeamComponent>(e).team_id == FpsLogic::kTeamAttackers) g_economy.awardPlantTeam(pv.get<Competitive::MoneyComponent>(e));
            sendEconomyState(pv.get<NetworkedEntity>(e).owner_client_id, e);
        }
        broadcastRoundState();
    }
    entt::entity defuser = entt::null;
    if (g_bomb_system.consumeDefuseCompleted(defuser) && defuser != entt::null && w->registry.valid(defuser)
        && w->registry.all_of<Competitive::MoneyComponent, NetworkedEntity>(defuser)) {
        g_economy.awardDefuse(w->registry.get<Competitive::MoneyComponent>(defuser));
        sendEconomyState(w->registry.get<NetworkedEntity>(defuser).owner_client_id, defuser);
    }
    // Detonation choice: no radial explosion damage; detonation ends the round
    // via IBombHooks -> RoundSystem TargetBombed (sufficient for Stage 3.4).
}
static void competitivePeriodReset(bool overtime)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    auto view = w->registry.view<NetworkedEntity, TeamComponent>();
    for (auto e : view) {
        auto& net = view.get<NetworkedEntity>(e);
        if (!net.is_player) continue;
        auto& team = view.get<TeamComponent>(e);
        // Halftime/OT-half: swap Attackers <-> Defenders (engine TeamComponent).
        if (team.team_id == FpsLogic::kTeamAttackers) team.team_id = FpsLogic::kTeamDefenders;
        else if (team.team_id == FpsLogic::kTeamDefenders) team.team_id = FpsLogic::kTeamAttackers;
        if (w->registry.all_of<Competitive::MoneyComponent>(e))
            w->registry.get<Competitive::MoneyComponent>(e).reset(
                g_round_system.config.currentStartMoney(overtime), g_round_system.config.money_cap);
        if (w->registry.all_of<ArmorComponent>(e)) w->registry.get<ArmorComponent>(e) = ArmorComponent{};
        // Strip weapons: pistol (team default) + knife only.
        if (w->registry.all_of<InventoryComponent, PlayerLoadoutComponent, WeaponComponent>(e)) {
            auto& inv = w->registry.get<InventoryComponent>(e);
            auto& lo = w->registry.get<PlayerLoadoutComponent>(e);
            auto& wpn = w->registry.get<WeaponComponent>(e);
            for (auto& s : lo.slots) s = WeaponSlotState{};
            WeaponType pistol = team.team_id == FpsLogic::kTeamAttackers ? WeaponType::GLOCK : WeaponType::P2000;
            const auto& pd = getWeaponDef(pistol);
            lo.slots[(size_t)WeaponSlot::Secondary] = WeaponSlotState{pistol, pd.max_ammo, pd.max_reserve_ammo, true};
            const auto& kd = getWeaponDef(WeaponType::KNIFE);
            lo.slots[(size_t)WeaponSlot::Melee] = WeaponSlotState{WeaponType::KNIFE, kd.max_ammo, kd.max_reserve_ammo, true};
            lo.active_slot = (int)WeaponSlot::Secondary;
            FpsLogic::loadSlotIntoWeapon(lo, wpn, lo.active_slot);
            FpsLogic::syncInventory(inv, lo);
            sendWeaponState(net.owner_client_id, e, wpn);
        }
        sendEconomyState(net.owner_client_id, e);
    }
    LOG_ENGINE_INFO("Competitive period reset (overtime={}): teams swapped, money reset", overtime);
}
static void competitiveRoundEndAwards(Competitive::RoundWinner winner, Competitive::RoundEndReason reason)
{
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    auto view = w->registry.view<NetworkedEntity, TeamComponent, Competitive::MoneyComponent>();
    for (auto e : view) {
        auto& net = view.get<NetworkedEntity>(e);
        if (!net.is_player) continue;
        auto& money = view.get<Competitive::MoneyComponent>(e);
        g_economy.awardRoundEnd(money, Competitive::teamFromId(view.get<TeamComponent>(e).team_id), winner, reason);
        sendEconomyState(net.owner_client_id, e);
    }
}
static const char* competitivePhaseName(Competitive::RoundPhase ph)
{
    using P = Competitive::RoundPhase;
    switch (ph) {
    case P::WaitingForPlayers: return "WaitingForPlayers";
    case P::Warmup: return "Warmup";
    case P::Freeze: return "Freeze";
    case P::Live: return "Live";
    case P::RoundEnd: return "RoundEnd";
    case P::Halftime: return "Halftime";
    case P::MatchEnded: return "MatchEnded";
    }
    return "?";
}
static void handleCompetitiveBuy(uint16_t client_id, BitReader& reader)
{
    // Type byte already consumed by the network dispatcher.
    uint32_t seq = reader.readUInt32();
    uint16_t item_id = reader.readUInt16();
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    entt::entity e = findPlayerEntity(*w, client_id);
    BuyResultMessage reply; reply.request_seq = seq; reply.item_id = item_id;
    if (e == entt::null || !w->registry.all_of<TeamComponent, Competitive::MoneyComponent, InventoryComponent, PlayerLoadoutComponent, WeaponComponent, FpsHealthComponent, TransformComponent>(e)) {
        reply.result = static_cast<uint8_t>(Competitive::BuyResult::Dead);
        BitWriter bw; CompetitiveSerializer::serialize(bw, reply);
        g_server_network.sendReliableToClient(client_id, bw);
        return;
    }
    uint8_t team = w->registry.get<TeamComponent>(e).team_id;
    // Buy zone: distance-to-team-spawn fallback (see CompetitiveLogic note).
    std::vector<glm::vec3> spawns;
    for (auto& sp : g_game_rules.getTeamSpawnPoints()) if (sp.team_id == team) spawns.push_back(sp.position);
    glm::vec3 pos = w->registry.get<TransformComponent>(e).position;
    Competitive::BuyContext ctx;
    ctx.client_id = client_id; ctx.request_seq = seq;
    ctx.phase = g_round_system.state.phase;
    ctx.seconds_into_live = g_round_system.state.phase == Competitive::RoundPhase::Live ? g_round_system.phaseElapsed() : 0.0f;
    ctx.alive = w->registry.get<FpsHealthComponent>(e).alive;
    ctx.in_buy_zone = Competitive::isInBuyZone(pos, spawns, g_round_system.config.buy_zone_radius);
    ctx.team_id = team;
    ctx.money = &w->registry.get<Competitive::MoneyComponent>(e);
    ctx.inventory = &w->registry.get<InventoryComponent>(e);
    ctx.loadout = &w->registry.get<PlayerLoadoutComponent>(e);
    ctx.weapon = &w->registry.get<WeaponComponent>(e);
    ctx.armor = w->registry.all_of<ArmorComponent>(e) ? &w->registry.get<ArmorComponent>(e) : nullptr;
    FpsLogic::DroppedItem dropped;
    // Replaced weapon (if any) is returned in `dropped`; COUNT = none.
    // Detect a replaced weapon so it can be dropped into the world.
    const WeaponCatalogDef* def = findCatalogById(item_id);
    Competitive::BuyResult result = g_buy_system.buy(ctx, item_id, &dropped);
    if (result == Competitive::BuyResult::Ok) {
        if (dropped.type != WeaponType::COUNT)
            spawnDroppedWeapon(*w, pos + glm::vec3(0.0f, 0.3f, 0.0f), dropped, client_id);
        if (def && def->kind != BuyItemKind::Equipment)
            sendWeaponState(client_id, e, *ctx.weapon);






    }
    reply.result = static_cast<uint8_t>(result);
    reply.balance = ctx.money->money();
    BitWriter bw; CompetitiveSerializer::serialize(bw, reply);
    g_server_network.sendReliableToClient(client_id, bw);
    sendEconomyState(client_id, e);
    LOG_ENGINE_INFO("Competitive buy client={} item={} result={} balance={}", client_id, item_id, (int)result, reply.balance);
}
static void handleCompetitiveTeamSelect(uint16_t client_id, BitReader& reader)
{
    uint8_t desired = reader.readByte();
    uint8_t auto_assign = reader.readByte();
    world* w = g_server_services ? g_server_services->game_world : nullptr;
    if (!w) return;
    entt::entity e = findPlayerEntity(*w, client_id);
    int32_t na = 0, nd = 0;
    auto tv = w->registry.view<NetworkedEntity, TeamComponent>();
    for (auto ent : tv) {
        if (!tv.get<NetworkedEntity>(ent).is_player) continue;
        uint8_t tid = tv.get<TeamComponent>(ent).team_id;
        if (tid == FpsLogic::kTeamAttackers) na++; else if (tid == FpsLogic::kTeamDefenders) nd++;
    }
    Competitive::FpsTeam out = Competitive::FpsTeam::None;
    Competitive::TeamSelectResult res = Competitive::validateTeamSelect(
        Competitive::teamFromId(desired), auto_assign != 0, na, nd,
        g_round_system.state.phase, g_round_system.config, out);
    if (res == Competitive::TeamSelectResult::Ok && e != entt::null && w->registry.all_of<TeamComponent>(e)) {
        uint8_t old = w->registry.get<TeamComponent>(e).team_id;
        w->registry.get<TeamComponent>(e).team_id = Competitive::teamId(out);
        LOG_ENGINE_INFO("Competitive team select client={} team {}->{}", client_id, (int)old, (int)out);
        if (Competitive::canSpawnNow(g_round_system.state.phase) &&
            w->registry.all_of<FpsHealthComponent>(e) && !w->registry.get<FpsHealthComponent>(e).alive) {
            respawnPlayer(client_id, g_game_rules.getSpawnForPlayer(*w, client_id, Competitive::teamId(out)));
        }
    }
    TeamResultMessage reply;
    reply.result = static_cast<uint8_t>(res);
    reply.team = static_cast<uint8_t>(out);
    BitWriter bw; CompetitiveSerializer::serialize(bw, reply);
    g_server_network.sendReliableToClient(client_id, bw);
}

// ---- Player spawning (called by network callbacks) ----

static void spawnPlayerForClient(uint16_t client_id)
{
    world* w = g_server_services->game_world;

    uint8_t team_id = g_game_rules.assignTeam(*w);
    if (g_competitive) {
        int32_t na = 0, nd = 0;
        auto tv = w->registry.view<NetworkedEntity, TeamComponent>();
        for (auto ent : tv) {
            if (!tv.get<NetworkedEntity>(ent).is_player) continue;
            uint8_t tid = tv.get<TeamComponent>(ent).team_id;
            if (tid == FpsLogic::kTeamAttackers) na++; else if (tid == FpsLogic::kTeamDefenders) nd++;
        }
        Competitive::FpsTeam assigned = Competitive::autoAssignTeam(na, nd);
        int32_t cnt = assigned == Competitive::FpsTeam::Attackers ? na : nd;
        team_id = cnt >= g_round_system.config.team_size
            ? Competitive::teamId(Competitive::FpsTeam::Spectator)
            : Competitive::teamId(assigned);
    }
    glm::vec3 spawn_pos = g_game_rules.getSpawnForPlayer(*w, client_id, team_id);
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

    CharacterControllerComponent controller;
    controller.move_speed = player.speed;
    controller.jump_velocity = player.jump_force;
    controller.capsule_half_height = player.capsule_half_height;
    controller.capsule_radius = player.capsule_radius;
    controller.mass = rigidbody.mass;
    controller.input_enabled = player.input_enabled;
    w->registry.emplace<CharacterControllerComponent>(player_entity, controller);

    // Add combat components
    w->registry.emplace<FpsHealthComponent>(player_entity, 100);
    w->registry.emplace<TeamComponent>(player_entity, TeamComponent{team_id});
    auto& inv_comp = w->registry.emplace<InventoryComponent>(player_entity);
    auto& loadout_comp = w->registry.emplace<PlayerLoadoutComponent>(player_entity);
    WeaponComponent weapon;
    FpsLogic::grantLoadout(inv_comp, loadout_comp, weapon, primaryForClient(client_id, team_id));
    auto& weapon_component = w->registry.emplace<WeaponComponent>(player_entity, weapon);
    w->registry.emplace<ScoreComponent>(player_entity);
    if (g_competitive) {
        auto& money = w->registry.emplace<Competitive::MoneyComponent>(player_entity);
        money.reset(g_round_system.config.start_money, g_round_system.config.money_cap);
        w->registry.emplace<ArmorComponent>(player_entity);
        // Late joiner (Live/RoundEnd) joins as dead observer, spawns next Freeze.
        if (!Competitive::canSpawnNow(g_round_system.state.phase)) {
            auto& h = w->registry.get<FpsHealthComponent>(player_entity);
            h.alive = false; h.health = 0;
        }
    }

    ensurePlayerPhysicsBody(*w, player_entity);

    g_server_network.setClientPlayerEntity(client_id, network_id);
    sendWeaponState(client_id, player_entity, weapon_component);

    // Broadcast spawn to all clients (camera_yaw is radians on the wire, D4)
    SpawnPlayerMessage spawn_msg;
    spawn_msg.client_id = client_id;
    spawn_msg.entity_id = network_id;
    spawn_msg.position = spawn_pos;
    spawn_msg.camera_yaw = glm::radians(transform.rotation.y);

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
        existing_msg.camera_yaw = glm::radians(trans.rotation.y);

        BitWriter existing_writer;
        NetworkSerializer::serialize(existing_writer, existing_msg);
        g_server_network.sendReliableToClient(client_id, existing_writer);
    }

    if (g_competitive) { broadcastRoundState(); sendEconomyState(client_id, player_entity); }
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
            if (g_competitive && g_bomb_system.bomb.carrier == entity && w->registry.all_of<TransformComponent>(entity))
                g_bomb_system.onCarrierDisconnect(w->registry.get<TransformComponent>(entity).position);
            g_bomb_use_held.erase(client_id); g_bomb_last_buttons.erase(client_id);
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
    {
        const char* mode_env = std::getenv("FPS_GAME_MODE");
        g_competitive = mode_env && std::strcmp(mode_env, "competitive") == 0;
        FpsLogic::ModeConfig cfg = (mode_env && std::strcmp(mode_env, "tdm") == 0)
            ? FpsLogic::teamDeathmatchConfig() : FpsLogic::deathmatchConfig();
        if (const char* lim = std::getenv("FPS_SCORE_LIMIT")) { int v = std::atoi(lim); if (v > 0) cfg.score_limit = v; }
        if (const char* tl = std::getenv("FPS_TIME_LIMIT")) { float v = (float)std::atof(tl); if (v > 0) cfg.time_limit_seconds = v; }
        g_game_rules.configureMode(cfg);
        if (g_competitive) {
            Competitive::ModeConfig ccfg; // all [P]/[V] values are data here
            if (const char* v = std::getenv("FPS_ROUND_SECONDS")) { float f = (float)std::atof(v); if (f > 0) ccfg.round_seconds = f; }
            if (const char* v = std::getenv("FPS_WARMUP_SECONDS")) { float f = (float)std::atof(v); if (f >= 0) ccfg.warmup_seconds = f; }
            g_round_system.configure(ccfg);
            g_round_system.bomb_hooks = &g_bomb_hooks;
            g_bomb_system.on_transition = [](Competitive::BombState from, Competitive::BombState to) {
                LOG_ENGINE_INFO("Bomb state {} -> {} carrier_valid={} timer={:.1f}", bombStateName(from), bombStateName(to),
                    g_bomb_system.bomb.carrier != entt::null, g_bomb_system.bomb.timer);
            };
            g_economy.config = Competitive::EconomyConfig{};
            g_bot_target_count = botCountFromEnv();
            g_buy_system.config = ccfg;
            g_round_system.on_phase_enter = [](Competitive::RoundPhase ph) {
                LOG_ENGINE_INFO("Competitive phase: {} round={} score A{}-D{} deadline_tick={}",
                    competitivePhaseName(ph), g_round_system.state.round_number,
                    (int)g_round_system.state.wins_attackers, (int)g_round_system.state.wins_defenders,
                    g_round_system.state.phase_deadline_tick);
                if (ph == Competitive::RoundPhase::Freeze) { competitiveFreezeRespawnAll(); competitiveFreezeBombSetup(); }
                broadcastRoundState();
            };
            g_round_system.on_period_reset = [](bool overtime) { competitivePeriodReset(overtime); };
            g_round_system.on_round_end = [](Competitive::RoundWinner winner, Competitive::RoundEndReason reason) {
                LOG_ENGINE_INFO("Competitive round end: winner={} reason={}", (int)winner, (int)reason);
                competitiveRoundEndAwards(winner, reason);
            };
            // Singleton MatchStateComponent entity.
            world* mw = services->game_world;
            g_match_entity = mw->registry.create();
            mw->registry.emplace<Competitive::MatchStateComponent>(g_match_entity, g_round_system.state);
            mw->registry.emplace<CountdownTimerComponent>(g_match_entity, g_round_system.timer);
            LOG_ENGINE_INFO("FPSShooter mode: competitive max_rounds={} clinch={} round={}s freeze={}s warmup={}s",
                ccfg.max_rounds, ccfg.winsToClinch(), ccfg.round_seconds, ccfg.freeze_seconds, ccfg.warmup_seconds);
        } else {
            LOG_ENGINE_INFO("FPSShooter mode: {} score_limit={} time_limit={}", cfg.mode == FpsLogic::FpsGameMode::TeamDeathmatch ? "team_deathmatch" : "deathmatch", cfg.score_limit, cfg.time_limit_seconds);
        }
    }

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
        if (!w->registry.all_of<FpsHealthComponent>(player_entity)) {
            return true;
        }
        if (g_competitive && g_round_system.isMovementLocked()) return false; // Freeze: movement locked
        return w->registry.get<FpsHealthComponent>(player_entity).alive;
    });

    g_server_network.setInputSampleHandler([](
        uint16_t client_id,
        entt::entity player_entity,
        const InputSample& input,
        uint32_t acknowledged_server_tick) {
        if (g_competitive) {
            g_bomb_use_held[client_id] = (input.buttons & InputFlags::USE) != 0;
            g_bomb_last_buttons[client_id] = input.buttons;
        }
        processWeaponInput(client_id, player_entity, input, acknowledged_server_tick);
    });

    // D3: weapon-select fires once per value change (engine edge detection).
    // Slot semantics are template data; switching lands with inventory.
    g_server_network.setWeaponSelectHandler([](
        uint16_t client_id, entt::entity player_entity, uint8_t weapon_select, const InputSample& input) {
        (void)input;
        doWeaponSelect(client_id, player_entity, weapon_select);
    });

    g_server_network.setOnClientConnected([](uint16_t client_id) {
        spawnPlayerForClient(client_id);
    });

    g_server_network.setOnClientDisconnected([](uint16_t client_id) {
        despawnPlayerForClient(client_id);
    });

    g_server_network.setCustomMessageHandler([](uint16_t client_id, uint8_t message_type, BitReader& reader) {
        if (g_competitive && message_type == static_cast<uint8_t>(CompetitiveMessageType::BUY_REQUEST)) {
            handleCompetitiveBuy(client_id, reader); return;
        }
        if (g_competitive && message_type == static_cast<uint8_t>(CompetitiveMessageType::TEAM_SELECT)) {
            handleCompetitiveTeamSelect(client_id, reader); return;
        }
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

    // D5 migration step 1: deliver events queued last frame on the server.
    // Timer ticking is owned by the host SystemScheduler (engine.timers,
    // fixed dt) and is intentionally not duplicated here.
    EventBus::get().flush();

    world* w = g_server_services->game_world;

    g_server_network.pumpNetworkEvents(delta_time);
    const uint32_t physics_steps = w->step_physics(delta_time);
    g_server_network.advanceSimulationTicks(physics_steps);
    updateServerPlayerCollisions(*w);
    if (g_competitive) {
        Competitive::RoundTickInput rin;
        auto pv = w->registry.view<NetworkedEntity, TeamComponent, FpsHealthComponent>();
        for (auto e : pv) {
            if (!pv.get<NetworkedEntity>(e).is_player) continue;
            uint8_t tid = pv.get<TeamComponent>(e).team_id;
            bool alive = pv.get<FpsHealthComponent>(e).alive;
            if (tid == FpsLogic::kTeamAttackers) { rin.attackers_connected++; if (alive) rin.attackers_alive++; }
            else if (tid == FpsLogic::kTeamDefenders) { rin.defenders_connected++; if (alive) rin.defenders_alive++; }
        }
        auto prev_phase = g_round_system.state.phase;
        if (g_bot_target_count > 0) { (void)loadBotNavMesh(); tickBots(w->fixed_delta > 0.0f ? w->fixed_delta : delta_time); }
        tickBombServer(w->fixed_delta > 0.0f ? w->fixed_delta : delta_time);
        g_round_system.tick(w->fixed_delta > 0.0f ? w->fixed_delta : delta_time, rin);
        (void)prev_phase;
        // ROUND_STATE heartbeat 1 Hz (spec S7); phase changes broadcast on enter.
        static float s_round_heartbeat = 0.0f;
        s_round_heartbeat += delta_time;
        if (s_round_heartbeat >= 1.0f) { s_round_heartbeat = 0.0f; broadcastRoundState(); }
    }
    g_game_rules.Update(*w, delta_time);

    // Update weapon cooldowns for all players
    auto weapon_view = w->registry.view<WeaponComponent, NetworkedEntity>();
    for (auto entity : weapon_view) {
        auto& weapon = weapon_view.get<WeaponComponent>(entity);
        auto& net = weapon_view.get<NetworkedEntity>(entity);
        const int32_t old_ammo = weapon.ammo;
        const int32_t old_reserve = weapon.reserve_ammo;
        const bool old_reloading = weapon.reloading;
        WeaponSystem::tick(weapon, w->fixed_delta);
        if ((old_ammo != weapon.ammo || old_reserve != weapon.reserve_ammo || old_reloading != weapon.reloading) &&
            net.owner_client_id != 0) {
            sendWeaponState(net.owner_client_id, entity, weapon);
        }
    }

    // Fall detection for server-side entities
    auto fall_view = w->registry.view<NetworkedEntity, TransformComponent, FpsHealthComponent>();
    for (auto entity : fall_view) {
        auto& trans = fall_view.get<TransformComponent>(entity);
        auto& health = fall_view.get<FpsHealthComponent>(entity);
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

            if (!g_competitive || g_round_system.state.phase == Competitive::RoundPhase::Warmup)
                g_game_rules.queueRespawn(net.owner_client_id);
        }
    }

    // Walk-over pickup: dropped weapon -> player if its slot is free (1.2 m).
    {
        std::vector<entt::entity> consumed;
        auto dv = w->registry.view<DroppedWeaponComponent, TransformComponent>();
        auto pv2 = w->registry.view<NetworkedEntity, TransformComponent, FpsHealthComponent, InventoryComponent, PlayerLoadoutComponent, WeaponComponent>();
        for (auto de : dv) {
            auto& dw = dv.get<DroppedWeaponComponent>(de);
            glm::vec3 dpos = dv.get<TransformComponent>(de).position;
            for (auto pe : pv2) {
                if (!pv2.get<FpsHealthComponent>(pe).alive) continue;
                if (glm::distance(pv2.get<TransformComponent>(pe).position, dpos) > 1.2f) continue;
                auto& inv3 = pv2.get<InventoryComponent>(pe);
                auto& lo3 = pv2.get<PlayerLoadoutComponent>(pe);
                auto& w3 = pv2.get<WeaponComponent>(pe);
                FpsLogic::DroppedItem item{dw.weapon_type, dw.ammo, dw.reserve};
                if (FpsLogic::tryPickup(inv3, lo3, w3, item)) {
                    uint16_t cid = pv2.get<NetworkedEntity>(pe).owner_client_id;
                    sendWeaponState(cid, pe, w3);
                    LOG_ENGINE_INFO("Player {} picked up weapon {}", cid, (int)dw.weapon_type);
                    consumed.push_back(de);
                    break;
                }
            }
        }
        for (auto de : consumed) if (w->registry.valid(de)) w->registry.destroy(de);
    }

    g_server_network.publishWorldState();

    EventBus::get().flush();
}

GAME_API void gardenServerOnLevelLoaded()
{
    if (!g_server_services) return;
    g_server_services->game_world->getPhysicsSystem().optimizeBroadPhase();
    // The host hashes Project.default_level and passes the selected artifact.
    g_server_network.setLevelHash(g_server_services->level_hash);
    (void)bakeBotNavMeshIfRequested();
    if (g_competitive && botCountFromEnv() > 0) { g_bot_target_count = botCountFromEnv(); (void)loadBotNavMesh(); }
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
