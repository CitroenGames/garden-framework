#include "Plugin/GameModuleAPI.h"
#include "Reflection/Reflect.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "InputActionSystem.hpp"
#include "LocalPlayerInputSystem.hpp"
#include "Components/Components.hpp"
#include "Components/LocalPlayerInputComponent.hpp"
#include "Components/camera.hpp"
#include "Network/ClientNetworkManager.hpp"
#include "Network/SharedMovement.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/CombatProtocol.hpp"
#include "shared/WeaponTypes.hpp"
#include "shared/WeaponSystem.hpp"
#include "shared/NetworkInputSampler.hpp"
#include "shared/MovementInputPolicy.hpp"
#include "shared/PlayerLifecycle.hpp"
#include "shared/FpsAnimationPresentation.hpp"
#include "GameHUD.hpp"
#include "ClientAcceptanceScript.hpp"
#include "ui/CombatFeedPresentation.hpp"
#include "ui/UiLogic.hpp"
#include "ui/UiInput.hpp"
#include "PlayerRepSystem.hpp"

#include "Utils/Log.hpp"
#include "UI/RmlUiManager.h"
#include "Console/ConVar.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Timer/TimerSystem.hpp"
#include "Debug/DebugDraw.hpp"
#include "Audio/AudioSystem.hpp"
#include "Audio/SoundEvent.hpp"
#include "Audio/AudioSourceSystem.hpp"
#include "ECS/Hierarchy.hpp"
#include "shared/FpsAudioEvents.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Threading/JobSystem.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

using Net::InputState;
using Net::MovementConfig;
using Net::MovementInput;
using Net::MovementState;
using Net::PredictionEntry;
namespace InputFlags = Net::InputFlags;
namespace SharedMovement = Net::SharedMovement;

static bool configureFPShooterInput(InputActionSystem& actions)
{
    const std::array<InputActionDef, 21> definitions = {{
        { "Move", ActionValueType::Axis2D, false },
        { "MoveForward", ActionValueType::Button, false },
        { "MoveBack", ActionValueType::Button, false },
        { "MoveLeft", ActionValueType::Button, false },
        { "MoveRight", ActionValueType::Button, false },
        { "Jump", ActionValueType::Button, false },
        { "Use", ActionValueType::Button, false },
        { "Sprint", ActionValueType::Button, false },
        { "Crouch", ActionValueType::Button, false },
        { "Walk", ActionValueType::Button, false },
        { "Drop", ActionValueType::Button, false },
        { "Fire", ActionValueType::Button, false },
        { "Reload", ActionValueType::Button, false },
        { "FreecamVertical", ActionValueType::Axis1D, false },
        { "FreecamFast", ActionValueType::Button, false },
        { "WeaponSlot1", ActionValueType::Button, false },
        { "WeaponSlot2", ActionValueType::Button, false },
        { "WeaponSlot3", ActionValueType::Button, false },
        { "WeaponSlot4", ActionValueType::Button, false },
        { "WeaponSlot5", ActionValueType::Button, false },
        { "Look", ActionValueType::Axis2D, false }
    }};

    for (const InputActionDef& definition : definitions)
    {
        if (!actions.registerAction(definition))
            return false;
    }

    MappingContext gameplay;
    gameplay.name = "Gameplay";
    gameplay.priority = 0;

    auto key = [&gameplay](const char* action, SDL_Scancode scancode, bool invert = false, uint8_t axis_target = 0) {
        gameplay.bindings.push_back({ action, InputBinding::Source::Key, scancode, 1.0f, invert, axis_target });
    };
    auto movementKey = [&gameplay](const char* button_action, SDL_Scancode scancode, bool invert, uint8_t axis_target) {
        gameplay.bindings.push_back({ "Move", InputBinding::Source::Key, scancode, 1.0f, invert, axis_target });
        gameplay.bindings.push_back({ button_action, InputBinding::Source::Key, scancode });
    };
    auto gamepadButton = [&gameplay](const char* action, SDL_GamepadButton button) {
        InputBinding binding;
        binding.action = action;
        binding.source = InputBinding::Source::GamepadButton;
        binding.code = button;
        binding.slot = 0;
        gameplay.bindings.push_back(std::move(binding));
    };
    auto gamepadAxis = [&gameplay](const char* action, SDL_GamepadAxis axis, bool invert = false,
                                   uint8_t axis_target = 0) {
        InputBinding binding;
        binding.action = action;
        binding.source = InputBinding::Source::GamepadAxis;
        binding.code = axis;
        binding.invert = invert;
        binding.axis_target = axis_target;
        binding.slot = 0;
        gameplay.bindings.push_back(std::move(binding));
    };

    // The vector action supports future analog movement; separate directional
    // buttons retain the legacy flags when opposite keys are held together.
    movementKey("MoveForward", SDL_SCANCODE_W, false, 1);
    movementKey("MoveBack", SDL_SCANCODE_S, true, 1);
    movementKey("MoveLeft", SDL_SCANCODE_A, true, 0);
    movementKey("MoveRight", SDL_SCANCODE_D, false, 0);
    gamepadAxis("Move", SDL_GAMEPAD_AXIS_LEFTX, false, 0);
    gamepadAxis("Move", SDL_GAMEPAD_AXIS_LEFTY, true, 1);
    gamepadAxis("MoveForward", SDL_GAMEPAD_AXIS_LEFTY, true);
    gamepadAxis("MoveBack", SDL_GAMEPAD_AXIS_LEFTY);
    gamepadAxis("MoveLeft", SDL_GAMEPAD_AXIS_LEFTX, true);
    gamepadAxis("MoveRight", SDL_GAMEPAD_AXIS_LEFTX);
    gamepadAxis("Look", SDL_GAMEPAD_AXIS_RIGHTX, false, 0);
    gamepadAxis("Look", SDL_GAMEPAD_AXIS_RIGHTY, false, 1);
    key("Jump", SDL_SCANCODE_SPACE);
    key("FreecamVertical", SDL_SCANCODE_SPACE);
    key("Use", SDL_SCANCODE_E);
    key("Sprint", SDL_SCANCODE_LSHIFT);
    key("FreecamVertical", SDL_SCANCODE_LSHIFT, true);
    key("FreecamFast", SDL_SCANCODE_LSHIFT);
    key("Crouch", SDL_SCANCODE_LCTRL);
    key("Crouch", SDL_SCANCODE_C);
    key("Walk", SDL_SCANCODE_LALT);
    key("Drop", SDL_SCANCODE_G);
    key("Reload", SDL_SCANCODE_R);
    key("WeaponSlot1", SDL_SCANCODE_1);
    key("WeaponSlot2", SDL_SCANCODE_2);
    key("WeaponSlot3", SDL_SCANCODE_3);
    key("WeaponSlot4", SDL_SCANCODE_4);
    key("WeaponSlot5", SDL_SCANCODE_5);
    gameplay.bindings.push_back({ "Fire", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT });
    gamepadButton("Jump", SDL_GAMEPAD_BUTTON_SOUTH);
    gamepadButton("Use", SDL_GAMEPAD_BUTTON_NORTH);
    gamepadButton("Sprint", SDL_GAMEPAD_BUTTON_LEFT_STICK);
    gamepadButton("Crouch", SDL_GAMEPAD_BUTTON_EAST);
    gamepadButton("Reload", SDL_GAMEPAD_BUTTON_WEST);
    gamepadAxis("Fire", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    return UiInput::registerModalContext(actions) && actions.pushContext(gameplay);
}

// The host hashes its actual Project.default_level artifact and carries the
// result in EngineServices, including for generated levels and package binaries.
static void applyLoadedLevelHash(Net::ClientNetworkManager& net, uint32_t level_hash)
{
    net.setLevelHash(level_hash);
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
static FPSShooter::ClientAcceptanceScript g_acceptance_script;

static entt::entity g_player_entity = entt::null;
static entt::entity g_local_input_entity = entt::null;
static entt::entity g_freecam_entity = entt::null;
static entt::entity g_player_rep_entity = entt::null;
static const LocalPlayerInputActionNames g_local_input_actions = {
    "Move", "Jump", "Sprint", "Crouch", "FreecamVertical", "FreecamFast", "Look"
};

static void setLocalInputOwner(world& game_world, entt::entity owner)
{
    setLocalPlayerInputOwner(
        game_world.registry, g_local_input_entity, owner, g_local_input_actions);
}

// Combat state
static Net::ReconciliationSmoothing g_recon_smoothing;
static int32_t g_local_health = 100;
static int32_t g_local_max_health = 100;
static bool g_local_alive = true;
static bool g_has_authoritative_health_snapshot = false;
static WeaponType g_local_weapon_type = WeaponType::RIFLE;
static int32_t g_local_ammo = 30;
static int32_t g_local_max_ammo = 30;
static int32_t g_local_reserve_ammo = 90;
static int32_t g_local_max_reserve_ammo = 90;
static bool g_local_reloading = false;
static bool g_has_authoritative_weapon_snapshot = false;
static float g_local_fire_cooldown = 0.0f;
static float g_local_accuracy_penalty = 0.0f;
static float g_local_recoil_index = 0.0f;
static float g_local_time_since_last_shot = 1000.0f;
static float g_local_weapon_spread = 0.0f;
static int32_t g_local_kills = 0;
static int32_t g_local_deaths = 0;
static float g_death_timer = 0.0f; // Countdown shown on death screen
static constexpr float RESPAWN_DELAY = 3.0f;

// PLAYER_DIED is the only current all-client feed publication. The richer
// server FpsCombatFeed has no corresponding wire message yet.
static UiLogic::CombatFeedPresentation g_combat_feed;
// Competitive client state (server-authoritative; client renders only).
static UiLogic::UiState g_ui;
static UiLogic::ConnectionLifecycle g_ui_connection;
static bool g_ui_modal_context_active = false;
static UiLogic::ClientCompetitiveState g_comp;
static std::vector<uint16_t> g_owned_items;
// Content has not supplied an authored radar calibration for the current
// template level.  Keep this explicitly unconfigured instead of deriving a
// misleading map from level bounds or client ECS.
static UiLogic::RadarMapTransform g_radar_map;
static UiLogic::RadarSlotState g_radar_slots;
static UiLogic::RadarPresentation g_radar;
static UiLogic::RadarThrottle g_radar_throttle;
static bool uiIsOwned(uint16_t id) { return std::find(g_owned_items.begin(), g_owned_items.end(), id) != g_owned_items.end(); }
static bool uiIsEquipped(const WeaponCatalogDef& def) { return def.weapon_type != WeaponType::COUNT && def.weapon_type == g_local_weapon_type; }
static uint32_t sendBuyRequest(uint16_t item_id) {
    BuyRequestMessage msg; msg.request_seq = g_comp.next_buy_seq++; msg.item_id = item_id;
    Net::BitWriter writer; CompetitiveSerializer::serialize(writer, msg);
    g_network.sendCustomReliable(writer);
    return msg.request_seq;
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

static void updateRadarPresentation(world& game_world, bool connected, float delta_time)
{
    if (!g_radar_throttle.shouldUpdate(delta_time))
        return;

    UiLogic::RadarPresentationSource source;
    source.connected = connected;
    source.competitive = &g_comp;
    source.map = g_radar_map;
    // Stable IDs are presenter-local keys only.  They are not network entity
    // IDs and this code never enumerates remote replicated entities.
    source.local_stable_id = static_cast<uint32_t>(g_network.getClientId()) + 1u;

    if (connected && game_world.registry.valid(g_player_entity) &&
        game_world.registry.all_of<TransformComponent>(g_player_entity)) {
        const TransformComponent& transform = game_world.registry.get<TransformComponent>(g_player_entity);
        UiLogic::RadarApprovedLocation self;
        self.stable_id = source.local_stable_id;
        self.kind = UiLogic::RadarLocationKind::LocalSelf;
        self.team = g_comp.local_team;
        self.alive = g_local_alive;
        self.world_x = transform.position.x;
        self.world_z = transform.position.z;
        source.approved_locations.push_back(self);
    }

    // No current custom message publishes teammate positions with a team-only
    // audience.  Do not substitute remote TransformComponents here: they may
    // include enemies and would turn generic replication into radar authority.
    source.has_authoritative_team_locations = false;
    g_radar = UiLogic::buildRadarPresentation(g_radar_slots, source);
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
    if (!services || !services->input_actions)
    {
        LOG_ENGINE_ERROR("The host did not provide the required input action service");
        return false;
    }
    g_services = services;
    g_ui_modal_context_active = false;
    if (!configureFPShooterInput(*services->input_actions))
    {
        LOG_ENGINE_ERROR("Failed to register FPSShooter input actions");
        g_services = nullptr;
        return false;
    }

    // Initialize networking
    if (!g_network.initialize()) {
        LOG_ENGINE_FATAL("Failed to initialize Client Network");
        return false;
    }

    const char* address = (services->connect_address && services->connect_address[0])
                          ? services->connect_address : "127.0.0.1";
    uint16_t port = services->connect_port ? services->connect_port : 7777;

    applyLoadedLevelHash(g_network, services->level_hash);
    if (!g_network.connectToServer(address, port, "Player")) {
        LOG_ENGINE_WARN("Failed to connect to server at {}:{} - running in offline mode", address, port);
    }

    if (!AudioSystem::get().isInitialized()) AudioSystem::get().initialize();
    registerFpsAudioEvents();

    g_network.setWorld(services->game_world);
    g_acceptance_script.loadFromEnvironment();

    g_network.setCustomMessageHandler([](uint8_t message_type, Net::BitReader& reader) {
        // Competitive messages (69-74): server-authoritative, client renders.
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE)) {
            RoundStateMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            auto prev = g_comp.phase;
            if (!UiLogic::applyRoundState(g_comp, msg.phase, msg.phase_time_remaining,
                                          msg.round_number, msg.wins_attackers, msg.wins_defenders,
                                          msg.bomb_planted != 0, msg.phase_deadline_tick, msg.server_tick))
                return;
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
            auto result = static_cast<Competitive::BuyResult>(msg.result);
            const bool accepted = UiLogic::applyAuthoritativeBuyReply(
                g_ui, g_comp, msg.item_id, result, msg.balance);
            if (accepted) {
                if (!uiIsOwned(msg.item_id)) g_owned_items.push_back(msg.item_id);
                if (msg.item_id == 42) g_comp.armor = 100;
                if (msg.item_id == 43) { g_comp.armor = 100; g_comp.has_helmet = true; }
                if (msg.item_id == 44) g_comp.has_kit = true;
            }
            g_acceptance_script.observeBuyResult(msg.request_seq, accepted);
            if (accepted) AudioEventSystem::get().play2D("ui.click");
            return;
        }
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::TEAM_RESULT)) {
            TeamResultMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            const bool accepted = UiLogic::applyAuthoritativeTeamReply(
                g_ui, static_cast<Competitive::TeamSelectResult>(msg.result),
                Competitive::teamFromId(msg.team), g_comp.local_team);
            g_acceptance_script.observeTeamResult(accepted, Competitive::teamFromId(msg.team));
            return;
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
            AudioEventSystem::get().playAt("weapon.fire", msg.ray_origin);
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
                g_local_alive = msg.health_remaining > 0;
                g_has_authoritative_health_snapshot = true;
                AudioEventSystem::get().play2D("player.hurt");
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
            if (!g_combat_feed.recordPlayerDied(msg.killer_client_id, msg.victim_client_id)) {
                LOG_ENGINE_WARN("Ignoring PLAYER_DIED with invalid victim client id");
                return;
            }

            if (msg.victim_client_id == g_network.getClientId()) {
                AudioEventSystem::get().play2D("player.death");
                g_local_alive = false;
                g_local_health = 0;
                g_has_authoritative_health_snapshot = true;
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
                g_has_authoritative_health_snapshot = true;
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
            const WeaponType received_weapon_type = static_cast<WeaponType>(msg.weapon_type);
            if (!hasWeaponDefinition(received_weapon_type)) {
                LOG_ENGINE_WARN("Ignoring WEAPON_STATE with invalid weapon type {}", msg.weapon_type);
                return;
            }
            g_local_weapon_type = received_weapon_type;
            g_local_ammo = msg.ammo;
            g_local_max_ammo = msg.max_ammo;
            g_local_reserve_ammo = msg.reserve_ammo;
            g_local_max_reserve_ammo = msg.max_reserve_ammo;
            g_local_reloading = msg.reloading != 0;
            g_local_fire_cooldown = std::max(g_local_fire_cooldown, msg.fire_cooldown);
            g_local_weapon_spread = msg.spread;
            g_local_accuracy_penalty = msg.accuracy_penalty;
            g_local_recoil_index = msg.recoil_index;
            g_has_authoritative_weapon_snapshot = true;
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
    if (g_services && g_services->game_world)
        setLocalInputOwner(*g_services->game_world, entt::null);
    ConVarRegistry::get().saveArchiveCvars("config.cfg");
    AudioSystem::get().shutdown();
    g_network.disconnect("Game closing");
    g_network.shutdown();
    g_acceptance_script.reset();
    g_player_controller.reset();
    g_hud.shutdown();
    if (g_services && g_services->input_actions)
    {
        g_services->input_actions->popContext(UiInput::ModalContextName);
        g_services->input_actions->popContext("Gameplay");
    }
    g_ui_modal_context_active = false;
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
    auto input_actions = g_services->input_actions;

    // Flush deferred events from previous frame
    EventBus::get().flush();

    // Update timers
    TimerSystem::get().update(delta_time);

    // Update debug draw
    DebugDraw::get().update(delta_time);

    // Network update
    g_network.update(delta_time);
    UiLogic::observeConnection(g_ui_connection, g_ui, g_network.isConnected());

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

    const entt::entity script_local_player = g_network.getLocalPlayerEntity();
    FPSShooter::ClientAcceptanceObservation acceptance_observation;
    acceptance_observation.connected = g_network.isConnected();
    acceptance_observation.has_round_state = g_comp.has_round_state;
    acceptance_observation.round_phase = g_comp.phase;
    acceptance_observation.local_team = g_comp.local_team;
    acceptance_observation.has_local_pawn = game_world->registry.valid(script_local_player);
    const FPSShooter::ClientAcceptanceDispatch acceptance_dispatch =
        g_acceptance_script.tick(delta_time, acceptance_observation);
    if (acceptance_dispatch.kind == FPSShooter::ClientAcceptanceDispatch::Kind::Team) {
        sendTeamSelect(acceptance_dispatch.team, acceptance_dispatch.auto_assign);
    } else if (acceptance_dispatch.kind == FPSShooter::ClientAcceptanceDispatch::Kind::Buy) {
        g_acceptance_script.noteBuyRequestSent(sendBuyRequest(acceptance_dispatch.item_id));
    }

    // Resolve UI state before projecting intent or consuming mouse/camera input.
    // Raw InputManager shortcuts remain available while the gameplay action
    // context is blocked by the modal screen.
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
        if (g_ui.screen == UiLogic::UiScreen::Pause &&
            input_manager->is_key_pressed(SDL_SCANCODE_F10)) {
            UiLogic::tryOpen(g_ui, UiLogic::UiScreen::Settings);
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

    const bool ui_modal = UiLogic::cursorWanted(g_ui);
    if (input_actions && !UiInput::synchronizeModalContext(
            *input_actions, ui_modal, g_ui_modal_context_active))
    {
        LOG_ENGINE_ERROR("Failed to synchronize FPSShooter modal input context");
    }

    const entt::entity local_input_owner = g_network.isConnected()
        ? g_network.getLocalPlayerEntity()
        : g_player_entity;
    setLocalInputOwner(*game_world, local_input_owner);
    if (input_actions)
        updateLocalPlayerInputs(game_world->registry, *input_actions);

    if (ui_modal && game_world->registry.valid(g_local_input_entity))
    {
        UiInput::suppressGameplayIntent(
            game_world->registry.try_get<LocalPlayerInputComponent>(g_local_input_entity));
    }

    if (g_player_controller && input_manager)
    {
        const float mouse_dx = input_manager->get_mouse_delta_x();
        const float mouse_dy = input_manager->get_mouse_delta_y();
        if ((mouse_dx != 0.0f || mouse_dy != 0.0f) && !ui_modal)
            g_player_controller->handleMouseMotion(mouse_dy, mouse_dx);
    }
    if (g_player_controller && !ui_modal)
        g_player_controller->updatePlayerLook(delta_time);

    g_combat_feed.tick(delta_time);

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
    if (g_network.isConnected() && input_actions)
    {
        const uint32_t command_tick = g_network.beginInputCommandTick();

        // Disable PlayerController's built-in movement - SharedMovement handles it
        g_player_controller->setMovementEnabled(
            FPSShooter::isLocalControllerMovementEnabled(true));

        const bool movement_locked = g_comp.has_round_state &&
            FPSShooter::isPawnMovementLocked(g_comp.phase);
        const bool movement_allowed = FPSShooter::isMovementInputAllowed(
            g_local_alive, movement_locked);
        const auto& weapon_def = getWeaponDef(g_local_weapon_type);
        const bool can_fire = g_local_fire_cooldown <= 0.0f && !g_local_reloading && g_local_ammo > 0;
        const bool can_reload = !g_local_reloading && g_local_ammo < weapon_def.max_ammo && g_local_reserve_ammo > 0;
        InputState input_state = FPSShooter::sampleNetworkInput(
            *input_actions,
            game_world->world_camera.rotation.y,
            game_world->world_camera.rotation.x,
            movement_allowed,
            ui_modal,
            can_fire,
            can_reload);
        if (g_local_alive && !ui_modal)
            g_acceptance_script.applyInput(input_state, movement_allowed, can_fire, can_reload);

        // Predict local weapon controls; the server consumes the same input flags authoritatively.
        if ((input_state.buttons & InputFlags::ATTACK) != 0)
        {
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

        // Reload is sampled from the same named action as the network command.
        if ((input_state.buttons & InputFlags::RELOAD) != 0)
        {
            g_local_reloading = true;
            // Reload timer handled by server, client just shows the state
        }

        // Client-side prediction for movement
        if (movement_allowed &&
            game_world->registry.valid(g_player_entity) &&
            game_world->registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(g_player_entity))
        {
            auto& trans = game_world->registry.get<TransformComponent>(g_player_entity);
            auto& rb = game_world->registry.get<RigidBodyComponent>(g_player_entity);
            auto& pc = game_world->registry.get<PlayerComponent>(g_player_entity);

            MovementInput move_input = FPSShooter::toMovementInput(input_state);

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
                        g_player_entity, FPSShooter::toCharacterMoveInput(input), move_config.fixed_delta));
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
                    if (game_world->registry.all_of<CharacterControllerComponent>(g_player_entity))
                    {
                        // Restore the authoritative state once, then let the
                        // same CharacterVirtual retain its contacts across all
                        // unacknowledged input steps.
                        game_world->set_character_controller_state(
                            g_player_entity, toCharacterControllerState(server_state));
                        g_network.getInputHistory().forEachFrom(server_tick,
                            [&](const PredictionEntry& entry) {
                                replay_state = toMovementState(game_world->simulate_character_controller(
                                    g_player_entity, FPSShooter::toCharacterMoveInput(entry.input), move_config.fixed_delta));
                            });
                    }
                    else
                    {
                        replay_state = g_network.getInputHistory().replayFrom(
                            server_tick,
                            server_state,
                            [&](const MovementInput& input, const MovementState& start_state) {
                                return SharedMovement::simulate(input, start_state, move_config);
                            });
                    }

                    trans.position = replay_state.position;
                    rb.velocity = replay_state.velocity;
                    pc.grounded = replay_state.grounded;
                    pc.ground_normal = replay_state.ground_normal;

                    // Apply smooth visual correction instead of snap
                    g_recon_smoothing.onCorrection(old_display_pos, trans.position);
                }
            }

            // Decay visual smoothing offset each frame
            g_recon_smoothing.update(delta_time);
        }

        g_network.sendInputCommand(command_tick, input_state);
    }
    else
    {
        if (g_player_controller)
            g_player_controller->setMovementEnabled(
                FPSShooter::isLocalControllerMovementEnabled(false));
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

    // Interpolation writes only the remote entities' presentation poses.
    // Run it before representations copy their tracked source pose this frame.
    if (g_network.isConnected())
        g_network.interpolateRemoteEntities();

    // Source-backed animation parameters are copied to existing player
    // representations before animation presentation advances this frame.
    FPSShooter::updateAnimationParameters(game_world->registry);

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
    Hierarchy::updateTransformPropagation(game_world->registry);
    AudioSourceSystem::update(game_world->registry, delta_time);
    if (g_player_controller)
    {
        camera& active_camera = g_player_controller->getActiveCamera();
        AudioSystem::get().setListenerPosition(
            active_camera.getPosition(),
            active_camera.camera_forward(),
            active_camera.getUpVector());
        AudioEventSystem::get().setListenerPosition(active_camera.getPosition());
    }
    AudioEventSystem::get().reapFinished([](SoundHandle h) { return AudioSystem::get().isSoundPlaying(h); });
    AudioSystem::get().update();

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

        const std::string kill_feed_text = g_combat_feed.buildText();

        UiLogic::HudSourceState hud_source;
        hud_source.connected = g_network.isConnected();
        hud_source.has_local_pawn = hud_source.connected && game_world->registry.valid(g_player_entity);
        if (hud_source.has_local_pawn)
            hud_source.pawn_health = game_world->registry.try_get<FpsHealthComponent>(g_player_entity);
        hud_source.has_damage_snapshot = g_has_authoritative_health_snapshot;
        hud_source.damage_snapshot = FpsHealthComponent{g_local_max_health};
        hud_source.damage_snapshot.health = g_local_health;
        hud_source.damage_snapshot.alive = g_local_alive;
        hud_source.has_weapon_snapshot = g_has_authoritative_weapon_snapshot;
        hud_source.weapon_snapshot = makeLocalWeaponSnapshot();
        hud_source.competitive = &g_comp;
        const UiLogic::HudCoreBindings hud_core = UiLogic::deriveHudCoreBindings(hud_source);

        g_hud.update(fps, pos, speed, grounded,
                     hud_source.connected ? g_network.getStats().ping_ms : 0.0f,
                     hud_core, g_death_timer, g_local_kills, g_local_deaths,
                     kill_feed_text, g_local_weapon_spread);
        // Competitive HUD text.  The protocol has no roster snapshot, so the
        // scoreboard is explicit about that and contains only the local row
        // backed by owner/shared authoritative messages.
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
            UiLogic::ScoreboardPresentationSource scoreboard_source;
            scoreboard_source.connected = hud_source.connected;
            scoreboard_source.competitive = &g_comp;
            // Do not source remote scoreboard rows from local replicated ECS:
            // their team/stat/money fields have not been published for this
            // presentation.  A future roster message will set this true.
            scoreboard_source.has_authoritative_roster = false;
            UiLogic::ScoreboardRowInput local_row;
            local_row.name = "Player " + std::to_string(g_network.getClientId());
            local_row.kills = g_local_kills; local_row.deaths = g_local_deaths;
            local_row.money = g_comp.money; local_row.side = g_comp.local_team;
            local_row.is_local = true; local_row.alive = g_local_alive;
            // Local transport RTT is the one ping value currently available.
            local_row.ping = static_cast<int32_t>(std::max(0.0f, g_network.getStats().ping_ms));
            local_row.ping_available = hud_source.connected;
            scoreboard_source.approved_rows.push_back(std::move(local_row));
            scoreboard_text = UiLogic::buildScoreboardPresentation(scoreboard_source).text;
        }
        updateRadarPresentation(*game_world, hud_source.connected, delta_time);
        UiLogic::SettingsPresentation settings;
        settings.vsync = CVAR_BOOL(r_vsync);
        settings.fxaa = CVAR_BOOL(r_fxaa);
        settings.sensitivity = CVAR_FLOAT(sensitivity);
        const UiLogic::DocumentPresentation document = UiLogic::buildDocumentPresentation(g_ui, settings);
        g_hud.updateCompetitive(g_comp, hud_core, g_ui, buy_text, scoreboard_text, g_radar, document);
    }
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services) return;
    applyLoadedLevelHash(g_network, g_services->level_hash);

    world* game_world = g_services->game_world;
    setLocalInputOwner(*game_world, entt::null);

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

    const entt::entity local_input_owner = g_network.isConnected()
        ? g_network.getLocalPlayerEntity()
        : g_player_entity;
    if (g_network.isConnected() && game_world->registry.valid(local_input_owner))
        g_player_entity = local_input_owner;
    setLocalInputOwner(*game_world, local_input_owner);

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
    g_has_authoritative_health_snapshot = false;
    g_local_kills = 0;
    g_local_deaths = 0;
    g_death_timer = 0.0f;
    g_recon_smoothing.visual_offset = glm::vec3(0.0f);
    g_comp = UiLogic::ClientCompetitiveState{}; g_ui = UiLogic::UiState{}; g_ui_connection = {}; g_owned_items.clear();
    g_radar_map = {};
    g_radar_slots.reset();
    g_radar = {};
    g_radar_throttle.reset();
    if (g_services->input_actions)
        UiInput::synchronizeModalContext(*g_services->input_actions, false, g_ui_modal_context_active);
    g_combat_feed.clear();

    const auto& def = getWeaponDef(WeaponType::RIFLE);
    g_local_weapon_type = WeaponType::RIFLE;
    g_local_ammo = def.max_ammo;
    g_local_max_ammo = def.max_ammo;
    g_local_reserve_ammo = def.max_reserve_ammo;
    g_local_max_reserve_ammo = def.max_reserve_ammo;
    g_local_reloading = false;
    g_has_authoritative_weapon_snapshot = false;
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
