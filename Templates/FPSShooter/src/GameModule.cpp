#include "Plugin/GameModuleAPI.h"
#include "Reflection/Reflect.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include "Application.hpp"
#include "world.hpp"
#include "Character/CharacterPhysicsService.hpp"
#include "LevelManager.hpp"
#include "Assets/AssetManager.hpp"
#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "InputActionSystem.hpp"
#include "LocalPlayerInputSystem.hpp"
#include "Components/Components.hpp"
#include "Components/LocalPlayerInputComponent.hpp"
#include "Components/camera.hpp"
#include "Network/ClientNetworkManager.hpp"
#include "Network/PredictionSystem.hpp"
#include "Network/NetworkTypes.hpp"
#include "Network/SharedMovement.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/FpsReplication.hpp"
#include "shared/CombatProtocol.hpp"
#include "shared/CompetitivePresentationCache.hpp"
#include "shared/FpsCombatFeed.hpp"
#include "shared/WorldItemPresentation.hpp"
#include "shared/WeaponTypes.hpp"
#include "shared/WeaponSystem.hpp"
#include "shared/NetworkInputSampler.hpp"
#include "shared/MovementInputPolicy.hpp"
#include "shared/FpsMovementTuning.hpp"
#include "shared/PlayerLifecycle.hpp"
#include "shared/FpsAnimationPresentation.hpp"
#include "GameHUD.hpp"
#include "ClientAcceptanceScript.hpp"
#include "ui/CombatFeedPresentation.hpp"
#include "ui/CompetitivePresentationProjection.hpp"
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
#include "Audio/AudioEventRouter.hpp"
#include "Audio/AudioSourceSystem.hpp"
#include "ECS/Hierarchy.hpp"
#include "shared/FpsAudioEvents.hpp"
#include "shared/FpsAudioRouting.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Threading/JobSystem.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <utility>

using Net::InputState;
using Net::MovementConfig;
using Net::MovementInput;
using Net::MovementState;
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
static FPSShooter::FixedNetworkInputCadence g_client_input_cadence;
static GameHUD g_hud;
static FPSShooter::ClientAcceptanceScript g_acceptance_script;
// Latches supplied by the ordinary client reconciliation path. The acceptance
// driver observes them but does not introduce a separate acknowledgement wire path.
static bool g_has_authoritative_pawn_position = false;
static glm::vec3 g_authoritative_pawn_position(0.0f);
static uint64_t g_authoritative_pawn_snapshot_generation = 0;
static uint32_t g_last_authoritative_input_tick = 0;
static FpsLogic::WorldItemPresentationIndex g_world_item_presentation_entities;
static world* g_world_item_presentation_world = nullptr;
// One template-owned mesh is shared by all client-only dropped-item rows. It
// carries render data only; the server remains the sole world-item authority.
static std::shared_ptr<mesh> g_world_item_visual_mesh;
static bool g_world_item_visual_load_attempted = false;
static constexpr const char* k_world_item_visual_source =
    "assets/models/items/dropped_weapon_case.gltf";

static void resetWorldItemVisualResource()
{
    g_world_item_visual_mesh.reset();
    g_world_item_visual_load_attempted = false;
}

static std::shared_ptr<mesh> resolveWorldItemVisualMesh()
{
    if (g_world_item_visual_mesh)
        return g_world_item_visual_mesh;
    if (g_world_item_visual_load_attempted)
        return nullptr;

    g_world_item_visual_load_attempted = true;
    if (!g_services || !g_services->level_manager || !g_services->render_api) {
        LOG_ENGINE_WARN("WORLD_ITEM_VISUAL unavailable: missing LevelManager or render API");
        return nullptr;
    }

    const std::string source_path =
        Assets::AssetManager::get().resolveAssetPath(k_world_item_visual_source);
    std::shared_ptr<mesh> loaded_mesh;
    if (std::filesystem::exists(source_path)) {
        LevelEntity entity{};
        entity.name = "dropped_weapon_case_presentation";
        entity.type = EntityType::Renderable;
        entity.mesh_path = source_path;
        entity.culling = true;
        entity.transparent = false;
        entity.visible = true;
        entity.force_lod = -1;
        loaded_mesh = g_services->level_manager->loadMesh(entity, g_services->render_api);
    } else {
        std::filesystem::path cooked_path(source_path);
        cooked_path.replace_extension(".cmesh");
        if (!std::filesystem::exists(cooked_path)) {
            LOG_ENGINE_WARN("WORLD_ITEM_VISUAL unavailable: no source or cooked mesh for {}", source_path);
            return nullptr;
        }
        loaded_mesh = g_services->level_manager->loadCompiledMesh(
            cooked_path.string(), g_services->render_api);
    }

    if (!loaded_mesh) {
        LOG_ENGINE_WARN("WORLD_ITEM_VISUAL unavailable: LevelManager failed to load {}", source_path);
        return nullptr;
    }
    if (!loaded_mesh->is_valid) {
        LOG_ENGINE_WARN("WORLD_ITEM_VISUAL unavailable: loaded mesh is invalid for {}", source_path);
        return nullptr;
    }
    if (!loaded_mesh->isUploadedToGPU()) {
        LOG_ENGINE_WARN("WORLD_ITEM_VISUAL unavailable: mesh did not upload for {}", source_path);
        return nullptr;
    }

    g_world_item_visual_mesh = std::move(loaded_mesh);
    LOG_ENGINE_INFO("WORLD_ITEM_VISUAL loaded {}", source_path);
    return g_world_item_visual_mesh;
}

static void bindWorldItemVisual(entt::registry& registry, uint32_t world_item_id)
{
    const auto entry = g_world_item_presentation_entities.find(world_item_id);
    if (entry == g_world_item_presentation_entities.end() ||
        !registry.valid(entry->second) ||
        !registry.all_of<TransformComponent, DroppedWeaponPresentationComponent>(entry->second)) {
        return;
    }

    const std::shared_ptr<mesh> visual_mesh = resolveWorldItemVisualMesh();
    if (!visual_mesh)
        return;
    registry.get_or_emplace<MeshComponent>(entry->second).m_mesh = visual_mesh;
    registry.get_or_emplace<RenderableComponent>(entry->second);
}

// Acceptance-only receipts mirror successful application to the local
// presentation index. They never drive gameplay or send a client message.
static uint64_t g_world_item_receipt_generation = 0;
// Counts accepted owner-only WEAPON_STATE receipts for causal acceptance waits.
// It is declared with the receipt cache because a world-item removal records
// the exact owner-state boundary observed at its callback.
static uint64_t g_authoritative_weapon_state_generation = 0;
struct WorldItemAcceptanceUpsertReceipt
{
    uint64_t generation = 0;
    WeaponType weapon_type = WeaponType::COUNT;
};
static std::unordered_map<uint32_t, WorldItemAcceptanceUpsertReceipt> g_world_item_upsert_receipts;
struct WorldItemAcceptanceRemoveReceipt
{
    uint64_t generation = 0;
    uint64_t owner_weapon_state_generation_at_receipt = 0;
};
static std::unordered_map<uint32_t, WorldItemAcceptanceRemoveReceipt> g_world_item_remove_receipts;

// The action driver only needs a short, post-action receipt window. Keep this
// value-only cache bounded so ordinary combat cannot accumulate diagnostics.
static constexpr size_t kMaxClientAcceptanceCombatReceipts = 16;
static uint64_t g_combat_receipt_generation = 0;
static std::vector<FPSShooter::ClientAcceptanceDamageReceipt> g_damage_acceptance_receipts;
static std::vector<FPSShooter::ClientAcceptanceDeathReceipt> g_death_acceptance_receipts;
static FpsCombat::ClientCombatEventIdentityCache g_combat_event_identity_cache;
static AudioEventRouter g_audio_event_router(AudioEventSystem::get());

template <typename Receipt>
static void appendClientAcceptanceCombatReceipt(std::vector<Receipt>& receipts, Receipt receipt)
{
    if (receipts.size() == kMaxClientAcceptanceCombatReceipts)
        receipts.erase(receipts.begin());
    receipts.push_back(std::move(receipt));
}

static void clearWorldItemAcceptanceReceipts()
{
    g_world_item_receipt_generation = 0;
    g_world_item_upsert_receipts.clear();
    g_world_item_remove_receipts.clear();
    g_combat_receipt_generation = 0;
    g_damage_acceptance_receipts.clear();
    g_death_acceptance_receipts.clear();
    g_combat_event_identity_cache.clear();
    g_audio_event_router.clear();
}

static bool tryGetClientPresentationPosition(uint16_t client_id, glm::vec3& position)
{
    if (!g_services || !g_services->game_world || client_id == 0)
        return false;

    auto entities = g_services->game_world->registry.view<Net::NetworkedEntity, TransformComponent>();
    for (const entt::entity entity : entities) {
        if (entities.get<Net::NetworkedEntity>(entity).owner_client_id != client_id)
            continue;
        const auto world_position = Hierarchy::resolveSimulationWorldPosition(
            g_services->game_world->registry, entity);
        if (!world_position)
            return false;
        position = *world_position;
        return true;
    }
    return false;
}

static EntityRef g_player_entity{};
static EntityRef g_local_input_entity{};
static EntityRef g_freecam_entity{};
static entt::entity g_player_rep_entity = entt::null;
static const LocalPlayerInputActionNames g_local_input_actions = {
    "Move", "Jump", "Sprint", "Crouch", "FreecamVertical", "FreecamFast", "Look"
};

static void setLocalInputOwner(world& game_world, EntityRef owner)
{
    setLocalPlayerInputOwner(
        game_world, g_local_input_entity, owner, g_local_input_actions);
}

// Combat state
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

// The template receives versioned COMBAT_EVENT records and formats only events
// accepted by its bounded identity cache.
static UiLogic::CombatFeedPresentation g_combat_feed;
// Competitive client state (server-authoritative; client renders only).
static UiLogic::UiState g_ui;
static UiLogic::ConnectionLifecycle g_ui_connection;
static bool g_ui_modal_context_active = false;
static bool g_ui_input_mode_active = false;
static UiLogic::ClientCompetitiveState g_comp;
static std::vector<uint16_t> g_owned_items;
// P3 publication is cached as one bounded, validated receipt. This package
// intentionally stops before HUD/RML projection or gameplay authority.
static CompetitivePresentationSnapshotCache g_competitive_presentation_cache;

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

static void tickPredictedLocalWeapon(float fixed_delta)
{
    if (g_local_fire_cooldown > 0.0f)
        g_local_fire_cooldown = std::max(g_local_fire_cooldown - fixed_delta, 0.0f);
    g_local_time_since_last_shot += std::max(fixed_delta, 0.0f);
    const auto& local_weapon_def = getWeaponDef(g_local_weapon_type);
    g_local_accuracy_penalty = WeaponSystem::decayTowardZero(
        g_local_accuracy_penalty, fixed_delta, local_weapon_def.recovery_time);
    if (g_local_time_since_last_shot > local_weapon_def.fire_rate * local_weapon_def.recoil_decay_delay)
        g_local_recoil_index = WeaponSystem::decayTowardZero(
            g_local_recoil_index, fixed_delta,
            1.0f / std::max(local_weapon_def.recoil_decay_rate, 0.001f));
}

// Command minting, sampling, local prediction, and enqueueing stay together
// on the client fixed timeline. Camera/UI remain in gardenGameUpdate's frame
// path, so their responsiveness is independent of simulation cadence.
static void runFixedClientInputStep(world& game_world,
                                    const InputActionSystem& input_actions,
                                    bool ui_modal,
                                    const FPSShooter::NetworkInputEdges& edges,
                                    float fixed_delta)
{
    tickPredictedLocalWeapon(fixed_delta);

    if (!g_network.isConnected())
    {
        if (g_player_controller)
            g_player_controller->setMovementEnabled(
                FPSShooter::isLocalControllerMovementEnabled(false));
        return;
    }

    const uint32_t command_tick = g_network.beginInputCommandTick();
    if (g_player_controller)
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
        input_actions,
        game_world.world_camera.rotation.y,
        game_world.world_camera.rotation.x,
        movement_allowed,
        ui_modal,
        can_fire,
        can_reload,
        edges);
    if (g_local_alive && !ui_modal)
        g_acceptance_script.applyInput(input_state, movement_allowed, can_fire, can_reload);
    const bool acceptance_steering_move_emitted = g_local_alive && !ui_modal && movement_allowed &&
        g_acceptance_script.isMoveToLiveSteering() &&
        (input_state.buttons & InputFlags::MOVE_FORWARD) != 0;

    // Client prediction mirrors the command stream. The server remains
    // authoritative for ammo, reload, and movement correction.
    if ((input_state.buttons & InputFlags::ATTACK) != 0)
    {
        --g_local_ammo;
        g_local_fire_cooldown = weapon_def.fire_rate;
        g_local_accuracy_penalty = std::min(
            g_local_accuracy_penalty + weapon_def.shot_spread_penalty, weapon_def.max_spread);
        g_local_recoil_index += 1.0f;
        g_local_time_since_last_shot = 0.0f;
        if (g_local_ammo <= 0 && g_local_reserve_ammo > 0)
            g_local_reloading = true;
    }
    if ((input_state.buttons & InputFlags::RELOAD) != 0)
        g_local_reloading = true;

    const EntityRef prediction_pawn = g_network.getLocalPlayerEntityRef();
    auto* trans = game_world.tryGetComponent<TransformComponent>(prediction_pawn);
    auto* rb = game_world.tryGetComponent<RigidBodyComponent>(prediction_pawn);
    auto* pc = game_world.tryGetComponent<PlayerComponent>(prediction_pawn);
    if (movement_allowed && trans && rb && pc)
    {
        const MovementInput move_input = FPSShooter::toMovementInput(input_state);
        MovementState move_state;
        move_state.position = trans->position;
        move_state.velocity = rb->velocity;
        move_state.grounded = pc->grounded;
        move_state.ground_normal = pc->ground_normal;

        MovementConfig move_config;
        move_config.speed = pc->speed;
        move_config.jump_force = pc->jump_force;
        move_config.fixed_delta = fixed_delta;

        CharacterPhysicsService character_physics(game_world);
        auto apply_movement = [&](const MovementInput& input, const MovementState& start_state) {
            CharacterControllerState controller_state;
            if (character_physics.hasController(prediction_pawn))
            {
                if (!character_physics.readControllerState(prediction_pawn, controller_state))
                    return start_state;
                // Keep controller jump history while updating the predicted pose.
                // Rebuilding the state from the wire-facing movement fields here
                // discards held-button and jump timer state on every command.
                controller_state.position = start_state.position;
                controller_state.velocity = start_state.velocity;
                controller_state.grounded = start_state.grounded;
                controller_state.ground_normal = start_state.ground_normal;
                controller_state.water_level = start_state.water_level;
                if (character_physics.writeControllerState(prediction_pawn, controller_state) &&
                    character_physics.simulateController(
                        prediction_pawn, FPSShooter::toCharacterMoveInput(input), fixed_delta, controller_state))
                    return toMovementState(controller_state);
                return start_state;
            }
            return SharedMovement::simulate(input, start_state, move_config);
        };

        const MovementState result = apply_movement(move_input, move_state);
        g_network.storeInput(command_tick, move_input, result);
        trans->position = result.position;
        rb->velocity = result.velocity;
        pc->grounded = result.grounded;
        pc->ground_normal = result.ground_normal;

        MovementState server_state;
        uint32_t server_tick;
        if (g_network.popAuthoritativeUpdate(server_state, server_tick))
        {
            // Keep the received local-pawn pose apart from the replayed
            // Transform. Scripted acceptance can therefore wait for the same
            // ordinary reconciliation truth the player receives.
            g_has_authoritative_pawn_position = true;
            g_authoritative_pawn_position = server_state.position;
            ++g_authoritative_pawn_snapshot_generation;
            g_last_authoritative_input_tick = server_tick;
            Net::PredictionReplayAdapter replay_adapter;
            if (character_physics.hasController(prediction_pawn)) {
                replay_adapter.restore_authoritative = [](world& replay_world, EntityRef pawn,
                                                           const MovementState& state) {
                    CharacterPhysicsService replay_physics(replay_world);
                    return replay_physics.writeControllerState(pawn, toCharacterControllerState(state));
                };
                replay_adapter.simulate = [fixed_delta](world& replay_world, EntityRef pawn,
                                                        const MovementInput& input,
                                                        const MovementState& state) {
                    CharacterPhysicsService replay_physics(replay_world);
                    CharacterControllerState controller_state;
                    if (replay_physics.simulateController(
                            pawn, FPSShooter::toCharacterMoveInput(input), fixed_delta, controller_state))
                        return toMovementState(controller_state);
                    return state;
                };
            } else {
                replay_adapter.simulate = [move_config](world&, EntityRef,
                                                        const MovementInput& input,
                                                        const MovementState& state) {
                    return SharedMovement::simulate(input, state, move_config);
                };
            }
            (void)Net::PredictionSystem::reconcile(
                game_world, prediction_pawn, server_state, server_tick, replay_adapter);
        }
    }

    g_network.sendInputCommand(command_tick, input_state);
    g_acceptance_script.noteInputCommandSent(
        command_tick, acceptance_steering_move_emitted, g_authoritative_weapon_state_generation);
}

static void updateRadarPresentation(bool connected, float delta_time)
{
    if (!g_radar_throttle.shouldUpdate(delta_time))
        return;
    const UiLogic::RadarPresentationSource source =
        UiLogic::projectCompetitivePresentationRadar(
            g_competitive_presentation_cache, connected, &g_comp, g_radar_map);
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
    if (!FPSShooter::Replication::registerReplicationComponents())
    {
        LOG_ENGINE_ERROR("Failed to register current FPSShooter replication descriptors");
        g_services = nullptr;
        return false;
    }
    g_ui_modal_context_active = false;
    g_ui_input_mode_active = false;
    g_client_input_cadence.reset();
    g_has_authoritative_pawn_position = false;
    g_authoritative_pawn_position = glm::vec3(0.0f);
    g_authoritative_pawn_snapshot_generation = 0;
    g_last_authoritative_input_tick = 0;
    g_world_item_presentation_entities.clear();
    resetWorldItemVisualResource();
    clearWorldItemAcceptanceReceipts();
    g_competitive_presentation_cache.clear();
    g_world_item_presentation_world = services->game_world;
    if (!configureFPShooterInput(*services->input_actions))
    {
        LOG_ENGINE_ERROR("Failed to register FPSShooter input actions");
        FPSShooter::Replication::unregisterReplicationComponents();
        g_services = nullptr;
        return false;
    }

    // Initialize networking
    if (!g_network.initialize()) {
        LOG_ENGINE_FATAL("Failed to initialize Client Network");
        FPSShooter::Replication::unregisterReplicationComponents();
        g_services = nullptr;
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
        if (message_type == static_cast<uint8_t>(CompetitivePresentationMessageType::SNAPSHOT)) {
            CompetitivePresentationSnapshotMessage message;
            if (!CompetitiveSerializer::deserialize(reader, message)) {
                LOG_ENGINE_WARN("Ignoring malformed COMPETITIVE_PRESENTATION snapshot");
                return;
            }

            const CompetitivePresentationSnapshotAcceptResult result =
                g_competitive_presentation_cache.accept(message);
            if (result == CompetitivePresentationSnapshotAcceptResult::RejectedInvalid ||
                result == CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision ||
                result == CompetitivePresentationSnapshotAcceptResult::RejectedStaleInstance) {
                LOG_ENGINE_INFO("Ignoring COMPETITIVE_PRESENTATION instance={} revision={} result={}",
                    message.match_instance_id, message.presentation_revision,
                    static_cast<unsigned>(result));
                return;
            }

            LOG_ENGINE_INFO("COMPETITIVE_PRESENTATION accepted instance={} revision={} rows={} radar={} result={}",
                message.match_instance_id, message.presentation_revision,
                static_cast<unsigned>(message.roster_count), static_cast<unsigned>(message.radar_count),
                static_cast<unsigned>(result));
            return;
        }
        // Competitive messages (69-74): server-authoritative, client renders.
        if (message_type == static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE)) {
            RoundStateMessage msg; if (!CompetitiveSerializer::deserialize(reader, msg)) return;
            auto prev = g_comp.phase;
            if (!UiLogic::applyRoundState(g_comp, msg.phase, msg.phase_time_remaining,
                                          msg.round_number, msg.wins_attackers, msg.wins_defenders,
                                          msg.bomb_planted != 0, msg.phase_deadline_tick, msg.server_tick,
                                          msg.last_reason))
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
            const Competitive::FpsTeam team = Competitive::teamFromId(msg.team);
            const bool accepted = UiLogic::applyAuthoritativeTeamReply(
                g_ui, static_cast<Competitive::TeamSelectResult>(msg.result), team, g_comp.local_team);
            g_acceptance_script.observeTeamResult(accepted, team);
            return;
        }
        if (message_type == static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_UPSERT)) {
            WorldItemUpsertMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Ignoring malformed WORLD_ITEM_UPSERT");
                return;
            }
            if (!g_services || !g_services->game_world ||
                g_world_item_presentation_world != g_services->game_world) {
                LOG_ENGINE_WARN("Ignoring WORLD_ITEM_UPSERT without an active client world");
                return;
            }
            const auto result = FpsLogic::upsertWorldItemPresentation(
                g_services->game_world->registry, g_world_item_presentation_entities, msg);
            if (result == FpsLogic::WorldItemPresentationUpsertResult::Rejected) {
                LOG_ENGINE_WARN("Ignoring invalid WORLD_ITEM_UPSERT id={}", msg.world_item_id);
                return;
            }
            bindWorldItemVisual(g_services->game_world->registry, msg.world_item_id);
            g_world_item_upsert_receipts[msg.world_item_id] = {
                ++g_world_item_receipt_generation, static_cast<WeaponType>(msg.weapon_type)
            };
            LOG_ENGINE_INFO(
                "WORLD_ITEM_UPSERT accepted id={} delivery={} result={} weapon={} pos=({:.2f},{:.2f},{:.2f})",
                msg.world_item_id,
                worldItemUpsertDeliveryName(static_cast<WorldItemUpsertDelivery>(msg.delivery)),
                result == FpsLogic::WorldItemPresentationUpsertResult::Created ? "created" : "updated",
                static_cast<unsigned>(msg.weapon_type), msg.position.x, msg.position.y, msg.position.z);
            return;
        }
        if (message_type == static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_REMOVE)) {
            WorldItemRemoveMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Ignoring malformed WORLD_ITEM_REMOVE");
                return;
            }
            if (!g_services || !g_services->game_world ||
                g_world_item_presentation_world != g_services->game_world) {
                LOG_ENGINE_WARN("Ignoring WORLD_ITEM_REMOVE without an active client world");
                return;
            }
            const auto result = FpsLogic::removeWorldItemPresentation(
                g_services->game_world->registry, g_world_item_presentation_entities, msg);
            if (result == FpsLogic::WorldItemPresentationRemoveResult::Rejected) {
                LOG_ENGINE_WARN("Ignoring invalid WORLD_ITEM_REMOVE id={}", msg.world_item_id);
                return;
            }
            if (result == FpsLogic::WorldItemPresentationRemoveResult::Removed) {
                g_world_item_remove_receipts[msg.world_item_id] = {
                    ++g_world_item_receipt_generation, g_authoritative_weapon_state_generation
                };
            }
            LOG_ENGINE_INFO("WORLD_ITEM_REMOVE accepted id={} result={}",
                msg.world_item_id,
                result == FpsLogic::WorldItemPresentationRemoveResult::Removed ? "removed" : "unknown_remove");
            return;
        }
        if (message_type == static_cast<uint8_t>(CombatEventMessageType::COMBAT_EVENT)) {
            CombatEventMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Ignoring malformed COMBAT_EVENT");
                return;
            }

            const FpsCombat::ClientCombatEventAcceptResult result =
                g_combat_event_identity_cache.accept(msg);
            if (result != FpsCombat::ClientCombatEventAcceptResult::Accepted) {
                LOG_ENGINE_INFO("Ignoring COMBAT_EVENT victim={} sequence={} result={}",
                    msg.victim_client_id, msg.death_sequence, static_cast<unsigned>(result));
                return;
            }
            LOG_ENGINE_INFO("COMBAT_EVENT accepted killer={} victim={} weapon={} flags={} death_sequence={} publication_sequence={}",
                msg.killer_client_id, msg.victim_client_id, msg.weapon_type, msg.flags,
                msg.death_sequence, msg.publication_sequence);
            if (!g_combat_feed.recordCombatEvent(msg)) {
                LOG_ENGINE_WARN("Ignoring COMBAT_EVENT rejected by feed presentation");
                return;
            }

            if (msg.victim_client_id == g_network.getClientId()) {
                appendClientAcceptanceCombatReceipt(g_death_acceptance_receipts,
                    FPSShooter::ClientAcceptanceDeathReceipt{
                        msg.killer_client_id, msg.victim_client_id, ++g_combat_receipt_generation
                    });
                g_local_alive = false;
                g_local_health = 0;
                g_has_authoritative_health_snapshot = true;
                g_local_deaths += msg.victim_deaths;
                g_death_timer = RESPAWN_DELAY;
                LOG_ENGINE_INFO("COMBAT_EVENT accepted victim={} sequence={} weapon={} flags={}",
                    msg.victim_client_id, msg.death_sequence, msg.weapon_type, msg.flags);
            }
            glm::vec3 death_position(0.0f);
            if (msg.victim_client_id == g_network.getClientId() ||
                tryGetClientPresentationPosition(msg.victim_client_id, death_position)) {
                (void)g_audio_event_router.route(
                    FpsAudio::makeDeathEvent(msg, g_network.getClientId(), death_position));
            }
            if (msg.killer_client_id == g_network.getClientId())
                g_local_kills += msg.killer_kills;
            return;
        }
        switch (static_cast<CombatMessageType>(message_type)) {
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
            (void)g_audio_event_router.route(
                FpsAudio::makeFireEvent(msg, g_network.getClientId()));
            break;
        }
        case CombatMessageType::DAMAGE_EVENT: {
            DamageEventMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize DAMAGE_EVENT");
                return;
            }
            (void)g_audio_event_router.route(
                FpsAudio::makeHurtEvent(msg, g_network.getClientId()));
            if (msg.victim_client_id == g_network.getClientId()) {
                appendClientAcceptanceCombatReceipt(g_damage_acceptance_receipts,
                    FPSShooter::ClientAcceptanceDamageReceipt{
                        msg.attacker_client_id, msg.victim_client_id, ++g_combat_receipt_generation
                    });
                g_local_health = msg.health_remaining;
                g_local_alive = msg.health_remaining > 0;
                g_has_authoritative_health_snapshot = true;
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
            // P1 retains PLAYER_DIED on the wire for its existing consumers,
            // but v5 clients derive idempotent presentation, local counts,
            // and acceptance receipts solely from the preceding COMBAT_EVENT.
            break;
        }
        case CombatMessageType::PLAYER_RESPAWN: {
            PlayerRespawnMessage msg;
            if (!CombatSerializer::deserialize(reader, msg)) {
                LOG_ENGINE_WARN("Failed to deserialize PLAYER_RESPAWN");
                return;
            }

            if (g_services && g_services->game_world) {
                world& game_world = *g_services->game_world;
                const EntityRef entity = g_network.getEntityRefByNetworkId(msg.entity_id);
                CharacterPhysicsService character_physics(game_world);
                if (character_physics.hasController(entity))
                    character_physics.teleportController(entity, msg.spawn_position);
                if (auto* transform = game_world.tryGetComponent<TransformComponent>(entity))
                    transform->position = msg.spawn_position;
                if (auto* rigid_body = game_world.tryGetComponent<RigidBodyComponent>(entity))
                    rigid_body->velocity = glm::vec3(0);
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
                Net::PredictionSystem::reset(*g_services->game_world, g_network.getLocalPlayerEntityRef());

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
            ++g_authoritative_weapon_state_generation;
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
    if (g_services && g_services->game_world) {
        if (g_world_item_presentation_world == g_services->game_world)
            FpsLogic::clearWorldItemPresentations(
                g_services->game_world->registry, g_world_item_presentation_entities);
        else
            g_world_item_presentation_entities.clear();
        g_world_item_presentation_world = nullptr;
        setLocalInputOwner(*g_services->game_world, EntityRef{});
    }
    // All presentation rows are gone before releasing their shared render resource.
    resetWorldItemVisualResource();
    ConVarRegistry::get().saveArchiveCvars("config.cfg");
    AudioSystem::get().shutdown();
    g_network.disconnect("Game closing");
    g_network.shutdown();
    FPSShooter::Replication::unregisterReplicationComponents();
    g_client_input_cadence.reset();
    g_acceptance_script.reset();
    g_has_authoritative_pawn_position = false;
    g_authoritative_pawn_position = glm::vec3(0.0f);
    g_authoritative_pawn_snapshot_generation = 0;
    g_last_authoritative_input_tick = 0;
    clearWorldItemAcceptanceReceipts();
    g_competitive_presentation_cache.clear();
    g_player_controller.reset();
    g_hud.shutdown();
    if (g_services && g_services->input_actions)
    {
        UiInput::synchronizeModalInput(
            *g_services->input_actions, g_services->input_mode_service, false,
            g_ui_modal_context_active, g_ui_input_mode_active);
        g_services->input_actions->popContext("Gameplay");
    }
    else if (g_services)
    {
        UiInput::synchronizeModalInputMode(
            g_services->input_mode_service, false, g_ui_input_mode_active);
    }
    g_ui_modal_context_active = false;
    g_ui_input_mode_active = false;
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
    if (UiLogic::observeConnection(g_ui_connection, g_ui, g_network.isConnected())) {
        g_competitive_presentation_cache.clear();
        g_audio_event_router.clear();
    }

    if (g_network.isConnected())
    {
        const EntityRef local_player = g_network.getLocalPlayerEntityRef();
        if (game_world->isEntityValid(local_player) && local_player != g_player_entity)
        {
            g_player_entity = local_player;
            if (g_player_controller)
                g_player_controller->setPossessedPlayer(g_player_entity);
        }
    }

    const EntityRef script_local_player = g_network.getLocalPlayerEntityRef();
    FPSShooter::ClientAcceptanceObservation acceptance_observation;
    acceptance_observation.connected = g_network.isConnected();
    acceptance_observation.local_client_id = g_network.getClientId();
    acceptance_observation.has_round_state = g_comp.has_round_state;
    acceptance_observation.bomb_planted = g_comp.bomb_planted;
    acceptance_observation.round_phase = g_comp.phase;
    acceptance_observation.last_round_end_reason = g_comp.last_round_end_reason;
    acceptance_observation.local_team = g_comp.local_team;
    acceptance_observation.has_local_pawn = game_world->isEntityValid(script_local_player);
    if (const auto* local_transform = game_world->tryGetComponent<TransformComponent>(script_local_player)) {
        acceptance_observation.has_local_transform = true;
        acceptance_observation.local_position_x = local_transform->position.x;
        acceptance_observation.local_position_z = local_transform->position.z;
    }
    acceptance_observation.has_authoritative_pawn_position = g_has_authoritative_pawn_position;
    acceptance_observation.authoritative_pawn_position_x = g_authoritative_pawn_position.x;
    acceptance_observation.authoritative_pawn_position_z = g_authoritative_pawn_position.z;
    acceptance_observation.authoritative_pawn_snapshot_generation =
        g_authoritative_pawn_snapshot_generation;
    acceptance_observation.last_authoritative_input_tick =
        g_last_authoritative_input_tick;
    // Remote player data comes from the ordinary client replication registry.
    // No server-only bot or target data is exposed to the acceptance driver.
    if (g_acceptance_script.active()) {
        const auto remote_players = game_world->registry.view<Net::NetworkedEntity, TransformComponent>();
        for (const entt::entity entity : remote_players) {
            const Net::NetworkedEntity& networked = remote_players.get<Net::NetworkedEntity>(entity);
            if (!networked.is_player || networked.owner_client_id == 0 ||
                networked.owner_client_id == acceptance_observation.local_client_id) {
                continue;
            }
            const TransformComponent& transform = remote_players.get<TransformComponent>(entity);
            acceptance_observation.remote_players.push_back({
                networked.owner_client_id, transform.position.x, transform.position.z
            });
        }
        std::sort(acceptance_observation.remote_players.begin(),
                  acceptance_observation.remote_players.end(),
                  [](const FPSShooter::ClientAcceptanceRemotePlayer& a,
                     const FPSShooter::ClientAcceptanceRemotePlayer& b) {
                      return a.client_id < b.client_id;
                  });
        acceptance_observation.damage_receipts = g_damage_acceptance_receipts;
        acceptance_observation.death_receipts = g_death_acceptance_receipts;
        acceptance_observation.combat_receipt_generation = g_combat_receipt_generation;
    }
    acceptance_observation.has_world_item_presentation_world =
        g_world_item_presentation_world == game_world;
    if (g_acceptance_script.active() && g_world_item_presentation_world == game_world) {
        acceptance_observation.world_item_receipt_generation = g_world_item_receipt_generation;
        acceptance_observation.world_items.reserve(g_world_item_presentation_entities.size());
        for (const auto& [world_item_id, entity] : g_world_item_presentation_entities) {
            const auto receipt = g_world_item_upsert_receipts.find(world_item_id);
            if (receipt == g_world_item_upsert_receipts.end() ||
                !game_world->registry.valid(entity) ||
                !game_world->registry.all_of<TransformComponent>(entity)) {
                continue;
            }
            const TransformComponent& transform = game_world->registry.get<TransformComponent>(entity);
            acceptance_observation.world_items.push_back({
                world_item_id, transform.position.x, transform.position.z, receipt->second.generation
            });
        }
        acceptance_observation.world_item_removals.reserve(g_world_item_remove_receipts.size());
        for (const auto& [world_item_id, receipt] : g_world_item_remove_receipts)
            acceptance_observation.world_item_removals.push_back({
                world_item_id, receipt.generation, receipt.owner_weapon_state_generation_at_receipt
            });
        acceptance_observation.world_item_weapon_receipts.reserve(g_world_item_upsert_receipts.size());
        for (const auto& [world_item_id, receipt] : g_world_item_upsert_receipts) {
            acceptance_observation.world_item_weapon_receipts.push_back({
                world_item_id, receipt.weapon_type, receipt.generation
            });
        }
    }
    acceptance_observation.has_authoritative_weapon_state = g_has_authoritative_weapon_snapshot;
    acceptance_observation.authoritative_weapon_type = g_local_weapon_type;
    acceptance_observation.authoritative_weapon_state_generation = g_authoritative_weapon_state_generation;
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
    if (input_actions && !UiInput::synchronizeModalInput(
            *input_actions, g_services->input_mode_service, ui_modal,
            g_ui_modal_context_active, g_ui_input_mode_active))
    {
        LOG_ENGINE_ERROR("Failed to synchronize FPSShooter modal input context");
    }

    const EntityRef local_input_owner = g_network.isConnected()
        ? g_network.getLocalPlayerEntityRef()
        : g_player_entity;
    setLocalInputOwner(*game_world, local_input_owner);
    const EntityRef movement_pawn = g_network.isConnected()
        ? g_network.getLocalPlayerEntityRef()
        : game_world->findEntityByTag("player");
    Net::PredictionSystem::configureMovement(
        *game_world, movement_pawn, FPSShooter::kMoveSpeed, FPSShooter::kJumpVelocity);
    if (input_actions)
        updateLocalPlayerInputs(*game_world, *input_actions);

    if (ui_modal && game_world->isEntityValid(g_local_input_entity))
    {
        UiInput::suppressGameplayIntent(
            game_world->tryGetComponent<LocalPlayerInputComponent>(g_local_input_entity));
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

    // Camera, HUD, and UI continue on the render cadence above. Only the
    // command sequence and predicted simulation consume this fixed clock.
    const float client_fixed_delta = game_world->fixed_delta;
    g_client_input_cadence.setFixedDelta(client_fixed_delta);
    if (input_actions)
        g_client_input_cadence.captureFrameEdges(*input_actions);

    if (!g_network.isConnected() && g_player_controller)
        g_player_controller->setMovementEnabled(
            FPSShooter::isLocalControllerMovementEnabled(false));

    const uint32_t fixed_steps = g_client_input_cadence.consumeFrame(delta_time);
    for (uint32_t step = 0; step < fixed_steps; ++step)
    {
        const FPSShooter::NetworkInputEdges edges = g_client_input_cadence.consumeEdgesForTick();
        if (input_actions)
            runFixedClientInputStep(*game_world, *input_actions, ui_modal, edges, client_fixed_delta);
        else
            tickPredictedLocalWeapon(client_fixed_delta);

        if (g_player_controller && !g_player_controller->isFreecamMode())
        {
            game_world->stepPhysicsSingleStep();
            CharacterPhysicsService character_physics(*game_world);
            character_physics.refreshController(g_player_entity);
        }
    }

    // Visual smoothing is presentation state and deliberately remains
    // frame-rate based even though its corrections are produced by fixed ticks.
    Net::PredictionSystem::updatePresentation(*game_world, g_network.getLocalPlayerEntityRef(), delta_time);

    // Update player controller
    if (g_player_controller)
        g_player_controller->update(delta_time);

    // Apply the prediction system's presentation-only correction to the camera.
    // Both poses are addressed through the Garden entity API rather than the
    // ECS storage implementation.
    if (g_network.isConnected())
    {
        const EntityRef prediction_pawn = g_network.getLocalPlayerEntityRef();
        MovementState logical_state;
        MovementState presentation_state;
        if (Net::PredictionSystem::getMovementState(*game_world, prediction_pawn, logical_state) &&
            Net::PredictionSystem::getPresentationState(*game_world, prediction_pawn, presentation_state))
        {
            const glm::vec3 offset = presentation_state.position - logical_state.position;
            game_world->world_camera.position += offset;
        }
    }

    // Interpolation writes only the remote entities' presentation poses.
    // Run it before representations copy their tracked source pose this frame.
    if (g_network.isConnected())
        g_network.interpolateRemoteEntities();

    // Source-backed animation parameters are copied to existing player
    // representations before animation presentation advances this frame.
    FPSShooter::updateAnimationParameters(game_world->registry);

    // Update player representations
    update_player_representations(*game_world,
        g_player_controller ? g_player_controller->isFreecamMode() : false);

    // Fall detection (client-side, for offline mode)
    if (!g_network.isConnected() && g_player_controller && !g_player_controller->isFreecamMode() &&
        game_world->hasComponent<TransformComponent>(g_player_entity))
    {
        auto& t = *game_world->tryGetComponent<TransformComponent>(g_player_entity);
        if (t.position.y < -50)
        {
            const glm::vec3 respawn_pos(0, 5, 0);
            CharacterPhysicsService character_physics(*game_world);
            if (game_world->hasComponent<CharacterControllerComponent>(g_player_entity))
                character_physics.teleportController(g_player_entity, respawn_pos);
            t.position = respawn_pos;
            if (game_world->hasComponent<RigidBodyComponent>(g_player_entity))
                game_world->tryGetComponent<RigidBodyComponent>(g_player_entity)->velocity = glm::vec3(0);
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
        if (game_world->hasComponent<TransformComponent>(g_player_entity))
        {
            auto& t = *game_world->tryGetComponent<TransformComponent>(g_player_entity);
            pos = t.position;
            if (game_world->hasComponent<RigidBodyComponent>(g_player_entity)) {
                const glm::vec3 velocity = game_world->tryGetComponent<RigidBodyComponent>(g_player_entity)->velocity;
                speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
            }
            if (game_world->hasComponent<PlayerComponent>(g_player_entity)) {
                const auto& player = *game_world->tryGetComponent<PlayerComponent>(g_player_entity);
                grounded = player.grounded;
                max_speed = player.speed;
            }
            if (game_world->hasComponent<CharacterControllerComponent>(g_player_entity)) {
                const auto& controller = *game_world->tryGetComponent<CharacterControllerComponent>(g_player_entity);
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
        hud_source.has_local_pawn = hud_source.connected && game_world->isEntityValid(g_player_entity);
        if (hud_source.has_local_pawn)
            hud_source.pawn_health = game_world->tryGetComponent<FpsHealthComponent>(g_player_entity);
        hud_source.has_damage_snapshot = g_has_authoritative_health_snapshot;
        hud_source.damage_snapshot = FpsHealthComponent{g_local_max_health};
        hud_source.damage_snapshot.health = g_local_health;
        hud_source.damage_snapshot.alive = g_local_alive;
        hud_source.has_weapon_snapshot = g_has_authoritative_weapon_snapshot;
        hud_source.weapon_snapshot = makeLocalWeaponSnapshot();
        hud_source.competitive = &g_comp;
        const UiLogic::HudCoreBindings hud_core = UiLogic::deriveHudCoreBindings(hud_source);
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
            const UiLogic::ScoreboardPresentationSource scoreboard_source =
                UiLogic::projectCompetitivePresentationScoreboard(
                    g_competitive_presentation_cache, hud_source.connected, &g_comp);
            scoreboard_text = UiLogic::buildScoreboardPresentation(scoreboard_source).text;
        }
        updateRadarPresentation(hud_source.connected, delta_time);
        UiLogic::SettingsPresentation settings;
        settings.vsync = CVAR_BOOL(r_vsync);
        settings.fxaa = CVAR_BOOL(r_fxaa);
        settings.sensitivity = CVAR_FLOAT(sensitivity);
        const UiLogic::DocumentPresentation document = UiLogic::buildDocumentPresentation(g_ui, settings);
        FpsHud::HudProjectionInput hud_view_input;
        hud_view_input.fps = fps;
        hud_view_input.position_x = pos.x;
        hud_view_input.position_y = pos.y;
        hud_view_input.position_z = pos.z;
        hud_view_input.speed = speed;
        hud_view_input.grounded = grounded;
        hud_view_input.ping = hud_source.connected ? g_network.getStats().ping_ms : 0.0f;
        hud_view_input.core = &hud_core;
        hud_view_input.competitive = &g_comp;
        hud_view_input.ui = &g_ui;
        hud_view_input.radar = &g_radar;
        hud_view_input.document = &document;
        hud_view_input.death_timer = g_death_timer;
        hud_view_input.kills = g_local_kills;
        hud_view_input.deaths = g_local_deaths;
        hud_view_input.weapon_spread = g_local_weapon_spread;
        hud_view_input.kill_feed = kill_feed_text;
        hud_view_input.buy_text = buy_text;
        hud_view_input.scoreboard_text = scoreboard_text;
        g_hud.updateView(FpsHud::projectHudView(hud_view_input));
    }
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services) return;
    applyLoadedLevelHash(g_network, g_services->level_hash);

    world* game_world = g_services->game_world;
    if (!game_world) return;
    if (g_world_item_presentation_world == game_world)
        FpsLogic::clearWorldItemPresentations(
            game_world->registry, g_world_item_presentation_entities);
    else
        g_world_item_presentation_entities.clear();
    // The old rows are clear before the shared visual is released. A later
    // upsert in this newly loaded level performs one bounded load attempt.
    resetWorldItemVisualResource();
    clearWorldItemAcceptanceReceipts();
    g_world_item_presentation_world = game_world;
    setLocalInputOwner(*game_world, EntityRef{});

    // Find player, freecam, and player rep entities
    g_player_entity = game_world->findEntityByTag("player");
    g_freecam_entity = game_world->findEntityByTag("freecam");
    g_player_rep_entity = entt::null;

    auto view = game_world->registry.view<TagComponent>();
    for (auto entity : view)
    {
        // Find by component type
        if (game_world->registry.all_of<PlayerRepresentationComponent>(entity) && g_player_rep_entity == entt::null)
            g_player_rep_entity = entity;
    }

    const EntityRef local_input_owner = g_network.isConnected()
        ? g_network.getLocalPlayerEntityRef()
        : g_player_entity;
    if (g_network.isConnected() && game_world->isEntityValid(local_input_owner))
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
    if (game_world->hasComponent<TransformComponent>(g_player_entity))
    {
        auto& t = *game_world->tryGetComponent<TransformComponent>(g_player_entity);
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
    Net::PredictionSystem::reset(*game_world, g_network.getLocalPlayerEntityRef());
    g_comp = UiLogic::ClientCompetitiveState{}; g_ui = UiLogic::UiState{}; g_ui_connection = {}; g_owned_items.clear();
    g_competitive_presentation_cache.clear();
    g_radar_map = {};
    g_radar_slots.reset();
    g_radar = {};
    g_radar_throttle.reset();
    if (g_services->input_actions)
        UiInput::synchronizeModalInput(
            *g_services->input_actions, g_services->input_mode_service, false,
            g_ui_modal_context_active, g_ui_input_mode_active);
    else
        UiInput::synchronizeModalInputMode(
            g_services->input_mode_service, false, g_ui_input_mode_active);
    g_combat_feed.clear();

    const auto& def = getWeaponDef(WeaponType::RIFLE);
    g_local_weapon_type = WeaponType::RIFLE;
    g_local_ammo = def.max_ammo;
    g_local_max_ammo = def.max_ammo;
    g_local_reserve_ammo = def.max_reserve_ammo;
    g_local_max_reserve_ammo = def.max_reserve_ammo;
    g_local_reloading = false;
    g_has_authoritative_weapon_snapshot = false;
    g_authoritative_weapon_state_generation = 0;
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
