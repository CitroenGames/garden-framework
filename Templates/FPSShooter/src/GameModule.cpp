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
#include "Network/ClientNetworkManager.hpp"
#include "Network/LevelHash.hpp"
#include "Network/SharedMovement.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/CombatProtocol.hpp"
#include "shared/WeaponTypes.hpp"
#include "shared/WeaponSystem.hpp"
#include "GameHUD.hpp"
#include "ui/UiLogic.hpp"
#include "PlayerRepSystem.hpp"

#include "Utils/Log.hpp"
#include "UI/RmlUiManager.h"
#include "Console/ConVar.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Timer/TimerSystem.hpp"
#include "Debug/DebugDraw.hpp"
#include "Audio/AudioSystem.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Threading/JobSystem.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <memory>

using Net::InputState;
using Net::MovementConfig;
using Net::MovementInput;
using Net::MovementState;
using Net::PredictionEntry;
namespace InputFlags = Net::InputFlags;
namespace SharedMovement = Net::SharedMovement;

static CharacterMoveInput toCharacterMoveInput(const MovementInput& input)
{
    CharacterMoveInput out;
    out.move_forward = input.move_forward;
    out.move_right = input.move_right;
    out.camera_yaw = input.camera_yaw;
    out.camera_pitch = input.camera_pitch;
    if ((input.buttons & InputFlags::JUMP) != 0)
        out.buttons |= CharacterMoveFlags::Jump;
    if ((input.buttons & InputFlags::CROUCH) != 0)
        out.buttons |= CharacterMoveFlags::Crouch;
    if ((input.buttons & InputFlags::SPRINT) != 0)
        out.buttons |= CharacterMoveFlags::Sprint;
    return out;
}

// Compute the v3 level hash for the template's default level and hand it
// to the client network manager (D2 step 6). Missing file leaves hash 0
// (no-level sentinel; both sides at 0 accept with a server warning).
static void applyTemplateLevelHash(Net::ClientNetworkManager& net)
{
    static const char* kCandidates[] = {
        "assets/levels/main.level.json",
        "Templates/FPSShooter/assets/levels/main.level.json",
        "../Templates/FPSShooter/assets/levels/main.level.json",
    };
    for (const char* path : kCandidates) {
        FILE* file = std::fopen(path, "rb");
        if (!file) continue;
        std::vector<uint8_t> bytes;
        uint8_t chunk[8192];
        size_t read = 0;
        while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            bytes.insert(bytes.end(), chunk, chunk + read);
        }
        std::fclose(file);
        net.setLevelHash(Net::computeLevelHashFromBytes(bytes));
        return;
    }
}

static MovementState toMovementState(const CharacterControllerState& state)
{
    MovementState out;
    out.position = state.position;
    out.velocity = state.velocity;
    out.grounded = state.grounded;
    out.ground_normal = state.ground_normal;
    out.water_level = state.water_level;
    return out;
}

static CharacterControllerState toCharacterControllerState(const MovementState& state)
{
    CharacterControllerState out;
    out.position = state.position;
    out.velocity = state.velocity;
    out.grounded = state.grounded;
    out.ground_normal = state.ground_normal;
    out.water_level = state.water_level;
    return out;
}

// ---- Module state ----

static EngineServices* g_services = nullptr;
static std::unique_ptr<PlayerController> g_player_controller;
static Net::ClientNetworkManager g_network;
static GameHUD g_hud;

static entt::entity g_player_entity = entt::null;
static entt::entity g_freecam_entity = entt::null;
static entt::entity g_player_rep_entity = entt::null;

// Combat state
static Net::ReconciliationSmoothing g_recon_smoothing;
static int32_t g_local_health = 100;
static int32_t g_local_max_health = 100;
static bool g_local_alive = true;
static WeaponType g_local_weapon_type = WeaponType::RIFLE;
static int32_t g_local_ammo = 30;
static int32_t g_local_max_ammo = 30;
static int32_t g_local_reserve_ammo = 90;
static int32_t g_local_max_reserve_ammo = 90;
static bool g_local_reloading = false;
static float g_local_fire_cooldown = 0.0f;
static float g_local_accuracy_penalty = 0.0f;
static float g_local_recoil_index = 0.0f;
static float g_local_time_since_last_shot = 1000.0f;
static float g_local_weapon_spread = 0.0f;
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
// Competitive client state (server-authoritative; client renders only).
static UiLogic::UiState g_ui;
static UiLogic::ClientCompetitiveState g_comp;
static std::vector<uint16_t> g_owned_items;
static std::array<UiLogic::RadarBlip, UiLogic::kRadarSlots> g_radar{}; // D10: zero slots until teammate replication exists
static bool uiIsOwned(uint16_t id) { return std::find(g_owned_items.begin(), g_owned_items.end(), id) != g_owned_items.end(); }
static bool uiIsEquipped(const WeaponCatalogDef& def) { return def.weapon_type != WeaponType::COUNT && def.weapon_type == g_local_weapon_type; }
static void sendBuyRequest(uint16_t item_id) {
    BuyRequestMessage msg; msg.request_seq = g_comp.next_buy_seq++; msg.item_id = item_id;
    Net::BitWriter writer; CompetitiveSerializer::serialize(writer, msg);
    g_network.sendCustomReliable(writer);
}
static void sendTeamSelect(Competitive::FpsTeam desired, bool auto_assign) {
    TeamSelectMessage msg; msg.desired_team = static_cast<uint8_t>(desired); msg.auto_assign = auto_assign ? 1 : 0;
    Net::BitWriter writer; CompetitiveSerializer::serialize(writer, msg);
    g_network.sendCustomReliable(writer);
}

static WeaponComponent makeLocalWeaponSnapshot()
{
    WeaponComponent weapon;
    weapon.weapon_type = g_local_weapon_type;
    weapon.ammo = g_local_ammo;
    weapon.max_ammo = g_local_max_ammo;
    weapon.reserve_ammo = g_local_reserve_ammo;
    weapon.max_reserve_ammo = g_local_max_reserve_ammo;
    weapon.fire_cooldown = g_local_fire_cooldown;
    weapon.accuracy_penalty = g_local_accuracy_penalty;
    weapon.recoil_index = g_local_recoil_index;
    weapon.time_since_last_shot = g_local_time_since_last_shot;
    weapon.reloading = g_local_reloading;
    return weapon;
}

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

    applyTemplateLevelHash(g_network);
    if (!g_network.connectToServer(address, port, "Player")) {
        LOG_ENGINE_WARN("Failed to connect to server at {}:{} - running in offline mode", address, port);
    }

    g_network.setWorld(services->game_world);

    g_network.setCustomMessageHandler([](uint8_t message_type, Net::BitReader& reader) {
        // Competitive messages (69-74): server-authoritative, client renders.
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE)) {
            RoundStateMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            auto prev = g_comp.phase;
            UiLogic::applyRoundState(g_comp, msg.phase, msg.phase_time_remaining, msg.round_number, msg.wins_attackers, msg.wins_defenders, msg.bomb_planted != 0);
            if (g_comp.phase == Competitive::RoundPhase::MatchEnded) g_ui.screen = UiLogic::UiScreen::MatchEnd;
            else if (prev != g_comp.phase && g_ui.screen == UiLogic::UiScreen::Buy && !UiLogic::buyWindowOpen(g_comp)) {
                UiLogic::closeToHud(g_ui); UiLogic::showNotice(g_ui, "Buy window closed");
            }
            return;
        }
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::ECONOMY_STATE)) {
            EconomyStateMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            UiLogic::applyEconomyState(g_comp, msg.balance, msg.loss_streak); return;
        }
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::BUY_RESULT)) {
            BuyResultMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            g_comp.money = msg.balance;
            auto result = static_cast<Competitive::BuyResult>(msg.result);
            if (result == Competitive::BuyResult::Ok) {
                if (!uiIsOwned(msg.item_id)) g_owned_items.push_back(msg.item_id);
                if (msg.item_id == 42) g_comp.armor = 100;
                if (msg.item_id == 43) { g_comp.armor = 100; g_comp.has_helmet = true; }
                if (msg.item_id == 44) g_comp.has_kit = true;
            }
            UiLogic::applyBuyResult(g_ui, result); return;
        }
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::TEAM_RESULT)) {
            TeamResultMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            UiLogic::applyTeamResult(g_ui, static_cast<Competitive::TeamSelectResult>(msg.result),
                                     Competitive::teamFromId(msg.team), g_comp.local_team); return;
        }        switch (static_cast<CombatMessageType>(message_type)) {
        case CombatMessageType::SHOOT_RESULT: {
            ShootResultMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize SHOOT_RESULT");
                return;
            }
            glm::vec3 color = (msg.hit_entity_id != 0)
                ? glm::vec3(1.0f, 0.3f, 0.0f)
                : glm::vec3(1.0f, 1.0f, 0.5f);
            DebugDraw::get().drawLine(msg.ray_origin, msg.hit_position, color, 0.15f);
            break;
        }
        case CombatMessageType::DAMAGE_EVENT: {
            DamageEventMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize DAMAGE_EVENT");
                return;
            }
            if (msg.victim_client_id == g_network.getClientId()) {
                g_local_health = msg.health_remaining;
                LOG_ENGINE_INFO("Took {} damage! Health: {}", msg.damage, g_local_health);
            }
            break;
        }
        case CombatMessageType::PLAYER_DIED: {
            PlayerDiedMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize PLAYER_DIED");
                return;
            }
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
            break;
        }
        case CombatMessageType::PLAYER_RESPAWN: {
            PlayerRespawnMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize PLAYER_RESPAWN");
                return;
            }

            if (g_services && g_services->game_world) {
                entt::entity entity = g_network.getEntityByNetworkId(msg.entity_id);
                if (g_services->game_world->registry.valid(entity)) {
                    auto& registry = g_services->game_world->registry;
                    if (registry.all_of<CharacterControllerComponent>(entity)) {
                        g_services->game_world->teleport_character_controller(entity, msg.spawn_position);
                    }
                    if (registry.all_of<TransformComponent>(entity)) {
                        registry.get<TransformComponent>(entity).position = msg.spawn_position;
                    }
                    if (registry.all_of<RigidBodyComponent>(entity)) {
                        registry.get<RigidBodyComponent>(entity).velocity = glm::vec3(0);
                    }
                }
            }

            if (msg.client_id == g_network.getClientId()) {
                g_local_alive = true;
                g_local_health = msg.health;
                g_local_max_health = msg.health;
                g_death_timer = 0.0f;
                g_local_fire_cooldown = 0.0f;
                g_local_reloading = false;

                // Active weapon is restored by the server WEAPON_STATE that
                // follows respawn (loadout primary, not always Rifle).
                g_local_accuracy_penalty = 0.0f;
                g_local_recoil_index = 0.0f;
                g_local_time_since_last_shot = 1000.0f;
                g_local_weapon_spread = 0.0f;
                g_recon_smoothing.visual_offset = glm::vec3(0.0f);
    g_comp = UiLogic::ClientCompetitiveState{}; g_ui = UiLogic::UiState{}; g_owned_items.clear();

                LOG_ENGINE_INFO("Respawned with {} health", msg.health);
            }
            break;
        }
        case CombatMessageType::WEAPON_STATE: {
            WeaponStateMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize WEAPON_STATE");
                return;
            }
            const uint8_t weapon_idx = std::min<uint8_t>(msg.weapon_type, static_cast<uint8_t>(WeaponType::COUNT) - 1);
            g_local_weapon_type = static_cast<WeaponType>(weapon_idx);
            g_local_ammo = msg.ammo;
            g_local_max_ammo = msg.max_ammo;
            g_local_reserve_ammo = msg.reserve_ammo;
            g_local_max_reserve_ammo = msg.max_reserve_ammo;
            g_local_reloading = msg.reloading != 0;
            g_local_fire_cooldown = std::max(g_local_fire_cooldown, msg.fire_cooldown);
            g_local_weapon_spread = msg.spread;
            g_local_accuracy_penalty = msg.accuracy_penalty;
            g_local_recoil_index = msg.recoil_index;
            break;
        }
        default:
            LOG_ENGINE_WARN("Unhandled combat message type {}", static_cast<int>(message_type));
            break;
        }
    });

    // Initialize HUD
    if (RmlUiManager::get().isInitialized())
    {
        if (!g_hud.initialize("assets/ui/hud.rml"))
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

    if (g_network.isConnected())
    {
        entt::entity local_player = g_network.getLocalPlayerEntity();
        if (game_world->registry.valid(local_player) && local_player != g_player_entity)
        {
            g_player_entity = local_player;
            if (g_player_controller)
                g_player_controller->setPossessedPlayer(g_player_entity);
        }
    }

    if (g_player_controller && input_manager)
    {
        const float mouse_dx = input_manager->get_mouse_delta_x();
        const float mouse_dy = input_manager->get_mouse_delta_y();
        if ((mouse_dx != 0.0f || mouse_dy != 0.0f) && !UiLogic::cursorWanted(g_ui))
            g_player_controller->handleMouseMotion(mouse_dy, mouse_dx);
    }

    // UI timers + keyboard-first menus (hud_menus S1/S3/S4/S5). Modal input is gated below.
    UiLogic::tickUi(g_ui, delta_time);
    UiLogic::tickCompetitive(g_comp, delta_time);
    if (g_ui.screen == UiLogic::UiScreen::Buy && g_comp.has_round_state && !UiLogic::buyWindowOpen(g_comp)) {
        UiLogic::closeToHud(g_ui); UiLogic::showNotice(g_ui, "Buy window closed");
    }
    if (input_manager) {
        UiLogic::setScoreboardHeld(g_ui, input_manager->is_key_held(SDL_SCANCODE_TAB));
        if (input_manager->is_key_pressed(SDL_SCANCODE_B)) {
            if (g_ui.screen == UiLogic::UiScreen::Buy) UiLogic::closeToHud(g_ui);
            else if (g_ui.screen == UiLogic::UiScreen::None) {
                if (UiLogic::canOpenBuy(g_comp)) UiLogic::tryOpen(g_ui, UiLogic::UiScreen::Buy);
                else UiLogic::showNotice(g_ui, g_comp.has_round_state ? "Buy window closed" : "Buy unavailable in this mode");
            }
        }
        if (input_manager->is_key_pressed(SDL_SCANCODE_M)) {
            if (g_ui.screen == UiLogic::UiScreen::TeamSelect) UiLogic::closeToHud(g_ui);
            else if (g_ui.screen == UiLogic::UiScreen::None) {
                if (UiLogic::canOpenTeamSelect(g_comp)) UiLogic::tryOpen(g_ui, UiLogic::UiScreen::TeamSelect);
                else UiLogic::showNotice(g_ui, "Team select unavailable in this mode");
            }
        }
        if (input_manager->is_key_pressed(SDL_SCANCODE_ESCAPE)) {
            if (g_ui.screen == UiLogic::UiScreen::Settings) UiLogic::backFromSettings(g_ui);
            else if (g_ui.screen != UiLogic::UiScreen::None) UiLogic::closeToHud(g_ui);
            else UiLogic::tryOpen(g_ui, UiLogic::UiScreen::Pause);
        }
        if (g_ui.screen == UiLogic::UiScreen::Buy) {
            static const SDL_Scancode digits[10] = {SDL_SCANCODE_1,SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4,SDL_SCANCODE_5,SDL_SCANCODE_6,SDL_SCANCODE_7,SDL_SCANCODE_8,SDL_SCANCODE_9,SDL_SCANCODE_0};
            for (int i = 0; i < 10; ++i) if (input_manager->is_key_pressed(digits[i])) sendBuyRequest(UiLogic::featuredBuyIds()[(size_t)i]);
        }
        if (g_ui.screen == UiLogic::UiScreen::TeamSelect) {
            if (input_manager->is_key_pressed(SDL_SCANCODE_1)) sendTeamSelect(Competitive::FpsTeam::Attackers, false);
            if (input_manager->is_key_pressed(SDL_SCANCODE_2)) sendTeamSelect(Competitive::FpsTeam::Defenders, false);
            if (input_manager->is_key_pressed(SDL_SCANCODE_3)) sendTeamSelect(Competitive::FpsTeam::Attackers, true);
            if (input_manager->is_key_pressed(SDL_SCANCODE_4)) sendTeamSelect(Competitive::FpsTeam::Spectator, false);
        }
    }
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

    // Update locally predicted weapon cooldown and accuracy recovery.
    if (g_local_fire_cooldown > 0.0f)
        g_local_fire_cooldown = std::max(g_local_fire_cooldown - delta_time, 0.0f);
    g_local_time_since_last_shot += std::max(delta_time, 0.0f);
    const auto& local_weapon_def = getWeaponDef(g_local_weapon_type);
    g_local_accuracy_penalty = WeaponSystem::decayTowardZero(
        g_local_accuracy_penalty, delta_time, local_weapon_def.recovery_time);
    if (g_local_time_since_last_shot > local_weapon_def.fire_rate * local_weapon_def.recoil_decay_delay)
        g_local_recoil_index = WeaponSystem::decayTowardZero(
            g_local_recoil_index, delta_time, 1.0f / std::max(local_weapon_def.recoil_decay_rate, 0.001f));

    // Send player input to server and run client-side prediction
    if (g_network.isConnected() && input_manager)
    {
        const uint32_t command_tick = g_network.beginInputCommandTick();

        // Disable PlayerController's built-in movement - SharedMovement handles it
        g_player_controller->setMovementEnabled(false);

        InputState input_state;
        input_state.buttons = 0;

        const bool ui_modal = UiLogic::cursorWanted(g_ui);
        if (g_local_alive && !ui_modal) {
            if (input_manager->is_key_held(SDL_SCANCODE_W)) input_state.buttons |= InputFlags::MOVE_FORWARD;
            if (input_manager->is_key_held(SDL_SCANCODE_S)) input_state.buttons |= InputFlags::MOVE_BACK;
            if (input_manager->is_key_held(SDL_SCANCODE_A)) input_state.buttons |= InputFlags::MOVE_LEFT;
            if (input_manager->is_key_held(SDL_SCANCODE_D)) input_state.buttons |= InputFlags::MOVE_RIGHT;
            if (input_manager->is_key_pressed(SDL_SCANCODE_SPACE)) input_state.buttons |= InputFlags::JUMP;
            if (input_manager->is_key_held(SDL_SCANCODE_E)) input_state.buttons |= InputFlags::USE;
            // D3 held posture / modifier bits (mirrors PlayerController mapping)
            if (input_manager->is_key_held(SDL_SCANCODE_LSHIFT)) input_state.buttons |= InputFlags::SPRINT;
            if (input_manager->is_key_held(SDL_SCANCODE_LCTRL) || input_manager->is_key_held(SDL_SCANCODE_C)) input_state.buttons |= InputFlags::CROUCH;
            if (input_manager->is_key_held(SDL_SCANCODE_LALT)) input_state.buttons |= InputFlags::WALK;
            // D3 edge actions: drop (G) and weapon select (digits 1-5, absolute slot)
            if (input_manager->is_key_pressed(SDL_SCANCODE_G)) input_state.buttons |= InputFlags::DROP;
            if (input_manager->is_key_pressed(SDL_SCANCODE_1)) input_state.buttons = Net::withWeaponSelect(input_state.buttons, 1);
            if (input_manager->is_key_pressed(SDL_SCANCODE_2)) input_state.buttons = Net::withWeaponSelect(input_state.buttons, 2);
            if (input_manager->is_key_pressed(SDL_SCANCODE_3)) input_state.buttons = Net::withWeaponSelect(input_state.buttons, 3);
            if (input_manager->is_key_pressed(SDL_SCANCODE_4)) input_state.buttons = Net::withWeaponSelect(input_state.buttons, 4);
            if (input_manager->is_key_pressed(SDL_SCANCODE_5)) input_state.buttons = Net::withWeaponSelect(input_state.buttons, 5);
        }

        input_state.camera_yaw = game_world->world_camera.rotation.y;
        input_state.camera_pitch = game_world->world_camera.rotation.x;
        input_state.move_forward = 0.0f;
        input_state.move_right = 0.0f;

        if (input_state.buttons & InputFlags::MOVE_FORWARD) input_state.move_forward += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_BACK) input_state.move_forward -= 1.0f;
        if (input_state.buttons & InputFlags::MOVE_RIGHT) input_state.move_right += 1.0f;
        if (input_state.buttons & InputFlags::MOVE_LEFT) input_state.move_right -= 1.0f;

        // Predict local weapon controls; the server consumes the same input flags authoritatively.
        if (g_local_alive && !ui_modal && input_manager->is_mouse_button_held(1) && g_local_fire_cooldown <= 0.0f && !g_local_reloading && g_local_ammo > 0)
        {
            const auto& weapon_def = getWeaponDef(g_local_weapon_type);

            input_state.buttons |= InputFlags::ATTACK;

            // Consume ammo locally (client prediction)
            g_local_ammo--;
            g_local_fire_cooldown = weapon_def.fire_rate;
            g_local_accuracy_penalty = std::min(
                g_local_accuracy_penalty + weapon_def.shot_spread_penalty, weapon_def.max_spread);
            g_local_recoil_index += 1.0f;
            g_local_time_since_last_shot = 0.0f;

            // Auto-reload
            if (g_local_ammo <= 0 && g_local_reserve_ammo > 0) {
                g_local_reloading = true;
            }
        }

        // Handle reload (R key)
        if (g_local_alive && !ui_modal && input_manager->is_key_pressed(SDL_SCANCODE_R) && !g_local_reloading) {
            const auto& weapon_def = getWeaponDef(g_local_weapon_type);
            if (g_local_ammo < weapon_def.max_ammo && g_local_reserve_ammo > 0) {
                input_state.buttons |= InputFlags::RELOAD;
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

            auto apply_movement = [&](const MovementInput& input, const MovementState& start_state) {
                if (game_world->registry.all_of<CharacterControllerComponent>(g_player_entity))
                {
                    game_world->set_character_controller_state(g_player_entity, toCharacterControllerState(start_state));
                    return toMovementState(game_world->simulate_character_controller(
                        g_player_entity, toCharacterMoveInput(input), move_config.fixed_delta));
                }
                return SharedMovement::simulate(input, start_state, move_config);
            };

            MovementState result = apply_movement(move_input, move_state);

            g_network.storeInput(command_tick, move_input, result);

            trans.position = result.position;
            rb.velocity = result.velocity;
            pc.grounded = result.grounded;
            pc.ground_normal = result.ground_normal;

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
                        replay_state = apply_movement(entry.input, replay_state);
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

        g_network.sendInputCommand(command_tick, input_state);
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
            const glm::vec3 respawn_pos(0, 5, 0);
            if (game_world->registry.all_of<CharacterControllerComponent>(g_player_entity))
                game_world->teleport_character_controller(g_player_entity, respawn_pos);
            t.position = respawn_pos;
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
        float max_speed = 10.0f;
        bool grounded = false;
        if (game_world->registry.valid(g_player_entity))
        {
            auto& t = game_world->registry.get<TransformComponent>(g_player_entity);
            pos = t.position;
            if (game_world->registry.all_of<RigidBodyComponent>(g_player_entity)) {
                const glm::vec3 velocity = game_world->registry.get<RigidBodyComponent>(g_player_entity).velocity;
                speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
            }
            if (game_world->registry.all_of<PlayerComponent>(g_player_entity)) {
                const auto& player = game_world->registry.get<PlayerComponent>(g_player_entity);
                grounded = player.grounded;
                max_speed = player.speed;
            }
            if (game_world->registry.all_of<CharacterControllerComponent>(g_player_entity)) {
                const auto& controller = game_world->registry.get<CharacterControllerComponent>(g_player_entity);
                grounded = controller.grounded;
                max_speed = controller.move_speed;
            }
        }

        WeaponSystem::AccuracyContext accuracy_context;
        accuracy_context.horizontal_speed = speed;
        accuracy_context.max_speed = max_speed;
        accuracy_context.grounded = grounded;
        g_local_weapon_spread = WeaponSystem::computeSpread(makeLocalWeaponSnapshot(), accuracy_context);

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
                     g_local_reserve_ammo, g_local_max_reserve_ammo,
                     g_local_alive, g_death_timer, g_local_kills, g_local_deaths,
                     kill_feed_text, g_local_reloading, g_local_weapon_spread);
        // Competitive HUD text (featured buy list + local scoreboard row only;
        // full roster/money snapshots are not replicated yet - no fabricated rows).
        std::string buy_text;
        if (g_ui.screen == UiLogic::UiScreen::Buy) {
            char line[160];
            for (size_t i = 0; i < UiLogic::featuredBuyIds().size(); ++i) {
                const WeaponCatalogDef* def = findCatalogById(UiLogic::featuredBuyIds()[i]);
                if (!def) continue;
                auto st = UiLogic::deriveBuyItemState(*def, g_comp.money, g_comp.local_team, g_comp.phase, g_comp.phase_elapsed, uiIsOwned(def->id), uiIsEquipped(*def));
                snprintf(line, sizeof(line), "%d %s $%d [%s]\n", (int)((i + 1) % 10), def->name, def->price, UiLogic::buyItemStateText(st));
                buy_text += line;
            }
        }
        std::string scoreboard_text;
        if (UiLogic::scoreboardVisible(g_ui)) {
            UiLogic::ScoreboardRowInput local_row;
            local_row.name = "Player " + std::to_string(g_network.getClientId());
            local_row.kills = g_local_kills; local_row.deaths = g_local_deaths;
            local_row.money = g_comp.money; local_row.side = g_comp.local_team; local_row.is_local = true; local_row.alive = g_local_alive;
            auto row = UiLogic::buildScoreboardRow(local_row, g_comp.local_team);
            char line[200];
            snprintf(line, sizeof(line), "%s K%d D%d money %d\n(full roster pending replication)", row.display_name.c_str(), row.kills, row.deaths, row.money_display);
            scoreboard_text = line;
        }
        g_hud.updateCompetitive(g_comp, g_ui, buy_text, scoreboard_text, g_radar);
    }
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services) return;
    applyTemplateLevelHash(g_network);

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
    g_comp = UiLogic::ClientCompetitiveState{}; g_ui = UiLogic::UiState{}; g_owned_items.clear();
    g_kill_feed.clear();

    const auto& def = getWeaponDef(WeaponType::RIFLE);
    g_local_weapon_type = WeaponType::RIFLE;
    g_local_ammo = def.max_ammo;
    g_local_max_ammo = def.max_ammo;
    g_local_reserve_ammo = def.max_reserve_ammo;
    g_local_max_reserve_ammo = def.max_reserve_ammo;
    g_local_reloading = false;
    g_local_fire_cooldown = 0.0f;
    g_local_accuracy_penalty = 0.0f;
    g_local_recoil_index = 0.0f;
    g_local_time_since_last_shot = 1000.0f;
    g_local_weapon_spread = def.spread;
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
