#pragma once

#include "Character/CharacterPhysicsService.hpp"
#include "InputActionSystem.hpp"
#include "InputManager.hpp"
#include "Network/PredictionSystem.hpp"
#include "shared/MovementInputPolicy.hpp"
#include "shared/FpsMovementTuning.hpp"
#include "shared/NetworkInputSampler.hpp"
#include "shared/PlayerLifecycle.hpp"
#include "shared/FpsAnimationPresentation.hpp"
#include "server/FpsDamage.hpp"

#include <string>
#include <limits>

bool testWorldTagLookupIdentity()
{
    const std::string name = "World_TagLookupIdentity";
    world game_world;
    auto& registry = game_world.registry;
    const entt::entity missing_identity = registry.create();
    registry.emplace<TagComponent>(missing_identity).name = "bare";
    const entt::entity null_identity = registry.create();
    registry.emplace<TagComponent>(null_identity).name = "null";
    registry.emplace<IdentityComponent>(null_identity, IdentityComponent{});
    const entt::entity valid = registry.create();
    registry.emplace<TagComponent>(valid).name = "valid";
    const EntityGuid guid{1, 2};
    registry.emplace<IdentityComponent>(valid, IdentityComponent{guid});
    const entt::entity duplicate = registry.create();
    registry.emplace<TagComponent>(duplicate).name = "valid";

    const auto count = registry.view<IdentityComponent>().size();
    const world& read_only_world = game_world;
    if (!read_only_world.findEntityByTag("").isNull() ||
        !read_only_world.findEntityByTag("missing").isNull() ||
        !read_only_world.findEntityByTag("bare").isNull() ||
        !read_only_world.findEntityByTag("null").isNull() ||
        !read_only_world.findEntityByTag("valid").isNull() ||
        registry.view<IdentityComponent>().size() != count ||
        registry.any_of<IdentityComponent>(missing_identity) ||
        registry.any_of<IdentityComponent>(duplicate) ||
        !registry.get<IdentityComponent>(null_identity).guid.isNull())
        return fail(name, "lookup changed identity storage or accepted an invalid match");

    registry.destroy(duplicate);
    const EntityRef found = read_only_world.findEntityByTag("valid");
    if (found.guid != guid || !read_only_world.isEntityValid(found) ||
        registry.view<IdentityComponent>().size() != count)
        return fail(name, "lookup did not return the existing valid identity");
    return pass(name);
}


bool testPlayerMovementTuning()
{
    const std::string name = "Player_MovementTuning";
    world game_world;
    const auto pawn_creation = game_world.createEntity();
    if (!settleTestRollback(game_world, pawn_creation.rollback))
        return fail(name, "entity rollback acknowledgement failed");
    if (pawn_creation.status != ScopedRestoreStatus::Completed)
        return fail(name, "pawn creation failed");
    const EntityRef pawn = pawn_creation.reference;
    const Net::MovementState initial_state;
    if (!Net::PredictionSystem::initializePawn(game_world, pawn, initial_state) ||
        !Net::PredictionSystem::configureMovement(game_world, pawn,
            FPSShooter::kMoveSpeed, FPSShooter::kJumpVelocity) ||
        Net::PredictionSystem::configureMovement(game_world, EntityRef{}, 4.5f, 3.0f) ||
        Net::PredictionSystem::configureMovement(game_world, pawn, -1.0f, 3.0f) ||
        Net::PredictionSystem::configureMovement(game_world, pawn, 4.5f,
                                                 std::numeric_limits<float>::quiet_NaN()))
        return fail(name, "controller configuration did not validate its handle and values");

    // Standalone levels locate their authored pawn by its template tag.
    LevelData level;
    LevelEntity level_player;
    level_player.name = "player";
    level_player.type = EntityType::Player;
    level_player.position = glm::vec3(0.0f, 2.0f, 0.0f);
    level.entities.push_back(level_player);
    world standalone_world;
    CharacterPhysicsService standalone_physics(standalone_world);
    standalone_world.initializePhysics();
    LevelManager level_manager;
    const auto level_result = level_manager.instantiateLevel(level, standalone_world, nullptr);
    if (!settleTestRollback(standalone_world, level_result.rollback))
        return fail(name, "level rollback acknowledgement failed");
    if (level_result.status != LevelInstantiationStatus::Completed)
        return fail(name, "standalone level did not instantiate");
    const EntityRef standalone_pawn = standalone_world.findEntityByTag("player");
    if (standalone_pawn.isNull() ||
        !standalone_physics.hasController(standalone_pawn) ||
        !Net::PredictionSystem::configureMovement(standalone_world, standalone_pawn,
            FPSShooter::kMoveSpeed, FPSShooter::kJumpVelocity) ||
        !standalone_world.findEntityByTag("missing").isNull())
        return fail(name, "standalone pawn lookup or movement configuration failed");

    PlayerComponent player;
    CharacterControllerComponent controller;
    FPSShooter::applyMovementTuning(player, controller);
    if (player.speed != controller.move_speed || player.jump_force != controller.jump_velocity)
        return fail(name, "player and controller tuning disagree");

    CharacterController::MovementTuning tuning;
    tuning.max_speed = controller.move_speed;
    tuning.jump_velocity = controller.jump_velocity;
    tuning.fixed_delta = 1.0f / 60.0f;
    tuning.sprint_speed_scale = controller.sprint_speed_scale;
    tuning.crouch_speed_scale = controller.crouch_speed_scale;
    tuning.stop_speed = tuning.max_speed * controller.stop_speed_ratio;
    tuning.air_wish_speed_cap = tuning.max_speed * controller.air_wish_speed_cap_ratio;
    auto groundSpeed = [&](uint16_t buttons) {
        CharacterControllerState state;
        state.grounded = true;
        CharacterMoveInput input;
        input.move_forward = 1.0f;
        input.buttons = buttons;
        for (int i = 0; i < 180; ++i) {
            state = CharacterController::simulateSourceMovement(input, state, tuning);
            state.grounded = true;
        }
        return glm::length(glm::vec2(state.velocity.x, state.velocity.z));
    };
    const float walk = groundSpeed(0);
    const float sprint = groundSpeed(CharacterMoveFlags::Sprint);
    const float crouch = groundSpeed(CharacterMoveFlags::Crouch);
    if (walk < 4.4f || walk > 4.6f || sprint < 6.6f || sprint > 6.9f ||
        crouch < 1.9f || crouch > 2.1f)
        return fail(name, "ground speed outside template bounds");

    CharacterControllerState state;
    state.grounded = true;
    CharacterMoveInput jump;
    jump.buttons = CharacterMoveFlags::Jump;
    state = CharacterController::simulateSourceMovement(jump, state, tuning);
    float apex = state.position.y;
    for (int i = 0; i < 120; ++i) {
        state = CharacterController::simulateSourceMovement(jump, state, tuning);
        state.grounded = false;
        apex = std::max(apex, state.position.y);
    }
    if (apex < 0.40f || apex > 0.55f)
        return fail(name, "held jump apex outside template bounds");
    return pass(name);
}

bool testPlayerPawnLifecycleContract()
{
    const std::string name = "Player_PawnLifecycleContract";

    entt::registry registry;
    const entt::entity pawn = registry.create();
    registry.emplace<FpsHealthComponent>(pawn, 100);
    registry.emplace<TeamComponent>(pawn, TeamComponent{FpsLogic::kTeamAttackers});

    Competitive::MatchStateComponent match;
    match.phase = Competitive::RoundPhase::Freeze;
    FPSShooter::synchronizePawnLifecycles(registry, match);
    auto& lifecycle = registry.get<FPSShooter::FpsPawnLifecycleComponent>(pawn);
    if (lifecycle.state != FPSShooter::FpsPawnLifecycleState::Active ||
        !FPSShooter::isPawnMovementLocked(match.phase) ||
        FPSShooter::isPawnMovementAllowed(registry.get<FpsHealthComponent>(pawn),
                                           registry.get<TeamComponent>(pawn), true, match.phase))
    {
        return fail(name, "spawned Freeze pawn did not retain an active, movement-locked lifecycle");
    }

    match.phase = Competitive::RoundPhase::Live;
    registry.get<FpsHealthComponent>(pawn).takeDamage(100);
    FPSShooter::synchronizePawnLifecycles(registry, match);
    if (lifecycle.state != FPSShooter::FpsPawnLifecycleState::Spectating ||
        FPSShooter::isPawnMovementAllowed(registry.get<FpsHealthComponent>(pawn),
                                           registry.get<TeamComponent>(pawn), true, match.phase))
    {
        return fail(name, "dead Live pawn did not transition to spectator with neutral movement");
    }

    match.phase = Competitive::RoundPhase::Freeze;
    FPSShooter::synchronizePawnLifecycles(registry, match);
    if (lifecycle.state != FPSShooter::FpsPawnLifecycleState::AwaitingSpawn)
        return fail(name, "dead playing-team pawn did not wait for the Freeze respawn boundary");

    registry.get<FpsHealthComponent>(pawn).reset();
    FPSShooter::synchronizePawnLifecycles(registry, match);
    if (lifecycle.state != FPSShooter::FpsPawnLifecycleState::Active)
        return fail(name, "Freeze respawn did not reactivate the pawn");

    registry.get<TeamComponent>(pawn).team_id = Competitive::teamId(Competitive::FpsTeam::Spectator);
    match.phase = Competitive::RoundPhase::Live;
    FPSShooter::synchronizePawnLifecycles(registry, match);
    if (lifecycle.state != FPSShooter::FpsPawnLifecycleState::Spectating ||
        FPSShooter::isPawnMovementAllowed(registry.get<FpsHealthComponent>(pawn),
                                           registry.get<TeamComponent>(pawn), true, match.phase))
        return fail(name, "spectator team retained a playable pawn lifecycle");

    registry.destroy(pawn);
    FPSShooter::synchronizePawnLifecycles(registry, match);
    const auto remaining = registry.view<FPSShooter::FpsPawnLifecycleComponent>();
    if (registry.valid(pawn) || remaining.begin() != remaining.end())
        return fail(name, "disconnect cleanup retained lifecycle state for a destroyed pawn");

    return pass(name);
}

bool testPlayerSpectatorStateContract()
{
    const std::string name = "Player_SpectatorStateContract";

    entt::registry registry;
    const entt::entity observer = registry.create();
    const entt::entity teammate = registry.create();
    const entt::entity enemy = registry.create();
    const entt::entity dead_teammate = registry.create();
    registry.emplace<FpsHealthComponent>(observer, 100).takeDamage(100);
    registry.emplace<FpsHealthComponent>(teammate, 100);
    registry.emplace<FpsHealthComponent>(enemy, 100);
    registry.emplace<FpsHealthComponent>(dead_teammate, 100).takeDamage(100);
    registry.emplace<TeamComponent>(observer, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<TeamComponent>(teammate, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<TeamComponent>(enemy, TeamComponent{FpsLogic::kTeamDefenders});
    registry.emplace<TeamComponent>(dead_teammate, TeamComponent{FpsLogic::kTeamAttackers});

    Competitive::MatchStateComponent match;
    match.phase = Competitive::RoundPhase::Live;
    FPSShooter::synchronizeSpectatorStates(registry, match);
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        !spectator.eligible || spectator.mode != FPSShooter::FpsSpectatorMode::Free ||
        !spectator.target.isNull()) {
        return fail(name, "dead player did not enter the eligible free observer state");
    }

    if (FPSShooter::setSpectatorChaseTarget(registry, observer, entt::null, match.phase) ||
        FPSShooter::setSpectatorChaseTarget(registry, observer, dead_teammate, match.phase) ||
        FPSShooter::setSpectatorChaseTarget(registry, observer, enemy, match.phase)) {
        return fail(name, "null, dead, or enemy chase target was accepted");
    }
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.mode != FPSShooter::FpsSpectatorMode::Free || !spectator.target.isNull())
        return fail(name, "invalid chase target changed spectator state");

    if (!FPSShooter::setSpectatorChaseTarget(registry, observer, teammate, match.phase))
        return fail(name, "living teammate did not become the chase target");
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.mode != FPSShooter::FpsSpectatorMode::Chase || spectator.target.isNull()) {
        return fail(name, "living teammate did not become the chase target");
    }

    registry.get<FpsHealthComponent>(teammate).takeDamage(100);
    FPSShooter::synchronizeSpectatorStates(registry, match);
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.mode != FPSShooter::FpsSpectatorMode::Free || !spectator.target.isNull())
        return fail(name, "target death did not clear the chase state");

    registry.get<FpsHealthComponent>(teammate).reset();
    if (!FPSShooter::setSpectatorChaseTarget(registry, observer, teammate, match.phase))
        return fail(name, "restored teammate could not become a chase target");
    registry.get<TeamComponent>(teammate).team_id = FpsLogic::kTeamDefenders;
    FPSShooter::synchronizeSpectatorStates(registry, match);
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.mode != FPSShooter::FpsSpectatorMode::Free || !spectator.target.isNull())
        return fail(name, "target team change did not clear the chase state");

    registry.get<TeamComponent>(teammate).team_id = FpsLogic::kTeamAttackers;
    if (!FPSShooter::setSpectatorChaseTarget(registry, observer, teammate, match.phase))
        return fail(name, "same-team target could not be restored before disconnect");
    registry.destroy(teammate);
    FPSShooter::synchronizeSpectatorStates(registry, match);
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.mode != FPSShooter::FpsSpectatorMode::Free || !spectator.target.isNull())
        return fail(name, "disconnected target did not clear the chase state");

    const entt::entity next_teammate = registry.create();
    registry.emplace<FpsHealthComponent>(next_teammate, 100);
    registry.emplace<TeamComponent>(next_teammate, TeamComponent{FpsLogic::kTeamAttackers});
    if (!FPSShooter::setSpectatorChaseTarget(registry, observer, next_teammate, match.phase))
        return fail(name, "new teammate could not become a chase target");
    match.phase = Competitive::RoundPhase::Freeze;
    FPSShooter::synchronizeSpectatorStates(registry, match);
    if (const auto& spectator = registry.get<FPSShooter::FpsSpectatorComponent>(observer);
        spectator.eligible || spectator.mode != FPSShooter::FpsSpectatorMode::Free ||
        !spectator.target.isNull()) {
        return fail(name, "Freeze round transition did not reset spectator state");
    }

    return pass(name);
}

bool testPlayerLocalSpawnSwitchKillRespawnContract()
{
    const std::string name = "Player_LocalSpawnSwitchKillRespawnContract";

    entt::registry registry;
    const entt::entity pawn = registry.create();
    const entt::entity attacker = registry.create();
    registry.emplace<FpsHealthComponent>(pawn, 100);
    registry.emplace<TeamComponent>(pawn, TeamComponent{FpsLogic::kTeamAttackers});
    auto& inventory = registry.emplace<InventoryComponent>(pawn);
    auto& loadout = registry.emplace<PlayerLoadoutComponent>(pawn);
    auto& weapon = registry.emplace<WeaponComponent>(pawn);
    FpsLogic::grantLoadout(inventory, loadout, weapon, WeaponType::RIFLE);
    registry.emplace<FPSShooter::FpsPawnLifecycleComponent>(pawn);
    auto& spectator = registry.emplace<FPSShooter::FpsSpectatorComponent>(pawn);
    spectator.eligible = true;
    spectator.mode = FPSShooter::FpsSpectatorMode::Chase;

    registry.emplace<FpsHealthComponent>(attacker, 100);
    registry.emplace<TeamComponent>(attacker, TeamComponent{FpsLogic::kTeamDefenders});

    // Save a non-default primary, switch away from it, then kill the pawn.
    // This is the same switch/drop sequence used by the server death resolver.
    weapon.ammo = 17;
    weapon.reserve_ammo = 43;
    if (!FpsLogic::switchToSlot(inventory, loadout, weapon, static_cast<int>(WeaponSlot::Secondary)) ||
        weapon.weapon_type != WeaponType::PISTOL ||
        !FpsLogic::switchToSlot(inventory, loadout, weapon, static_cast<int>(WeaponSlot::Primary))) {
        return fail(name, "spawned pawn could not preserve a switched primary state");
    }

    FpsServer::DamagePolicy policy;
    policy.require_opposing_teams = true;
    const auto lethal = FpsServer::applyFpsDamage(registry, {attacker, pawn, 100}, policy);
    const auto death = FpsServer::beginFpsDeathTransition(registry, pawn);
    if (!lethal.killed || !death.transitioned || !FpsLogic::switchToSlot(
            inventory, loadout, weapon, static_cast<int>(WeaponSlot::Primary))) {
        return fail(name, "lethal spawn state did not enter the death transition");
    }
    const auto dropped = FpsLogic::dropActive(inventory, loadout, weapon);
    if (!dropped || dropped->type != WeaponType::RIFLE || dropped->ammo != 17 ||
        dropped->reserve != 43 || weapon.weapon_type != WeaponType::PISTOL ||
        !FpsLogic::hasMirroredLoadoutInventory(inventory, loadout)) {
        return fail(name, "death did not keep switched weapon and inventory state coherent");
    }

    // The respawn boundary must immediately reopen health/death/loadout and
    // observer state as one local ECS transition, before any later world sync.
    const auto& restored_health = registry.get<FpsHealthComponent>(pawn);
    if (!FPSShooter::restorePawnAfterRespawn(registry, pawn, WeaponType::AK47) ||
        !restored_health.alive || restored_health.health != restored_health.max_health ||
        restored_health.invulnerable_timer <= 0.0f ||
        registry.get<FpsDeathLifecycleComponent>(pawn).resolved ||
        registry.get<FpsDeathLifecycleComponent>(pawn).sequence != death.death_sequence ||
        registry.get<FPSShooter::FpsPawnLifecycleComponent>(pawn).state !=
            FPSShooter::FpsPawnLifecycleState::Active ||
        registry.get<FPSShooter::FpsSpectatorComponent>(pawn).eligible ||
        registry.get<FPSShooter::FpsSpectatorComponent>(pawn).mode != FPSShooter::FpsSpectatorMode::Free ||
        weapon.weapon_type != WeaponType::AK47 ||
        !FpsLogic::hasMirroredLoadoutInventory(inventory, loadout)) {
        return fail(name, "respawn did not atomically restore local pawn state");
    }

    return pass(name);
}

bool testPlayerAnimationParameterContract()
{
    const std::string name = "Player_AnimationParameterContract";

    entt::registry registry;
    const entt::entity pawn = registry.create();
    auto& health = registry.emplace<FpsHealthComponent>(pawn, 100);
    auto& controller = registry.emplace<CharacterControllerComponent>(pawn);
    controller.grounded = true;
    auto& body = registry.emplace<RigidBodyComponent>(pawn);
    body.velocity = glm::vec3(3.0f, 0.0f, 4.0f);
    registry.emplace<TeamComponent>(pawn, TeamComponent{FpsLogic::kTeamAttackers});
    auto& loadout = registry.emplace<PlayerLoadoutComponent>(pawn);
    loadout.active_slot = static_cast<int32_t>(WeaponSlot::Primary);
    loadout.slots[static_cast<size_t>(WeaponSlot::Primary)].occupied = true;
    auto& weapon = registry.emplace<WeaponComponent>(pawn);
    weapon.fire_cooldown = 0.15f;

    FPSShooter::updateAnimationParameters(registry);
    const auto& moving = registry.get<FPSShooter::FpsAnimationParametersComponent>(pawn);
    if (!approx(moving.horizontal_speed, 5.0f) || moving.team_id != FpsLogic::kTeamAttackers ||
        moving.active_weapon_slot != static_cast<int32_t>(WeaponSlot::Primary) ||
        !moving.has_active_weapon || moving.idle || !moving.moving || moving.jumping ||
        moving.reloading || !moving.firing || moving.dead)
    {
        return fail(name, "grounded moving parameter mapping was not deterministic");
    }

    body.velocity = glm::vec3(0.0f);
    controller.grounded = false;
    weapon.reloading = true;
    FPSShooter::updateAnimationParameters(registry);
    const auto& airborne = registry.get<FPSShooter::FpsAnimationParametersComponent>(pawn);
    if (airborne.idle || airborne.moving || !airborne.jumping || !airborne.reloading ||
        airborne.firing || airborne.dead)
    {
        return fail(name, "airborne reload parameters did not override movement and fire");
    }

    health.takeDamage(100);
    FPSShooter::updateAnimationParameters(registry);
    const auto& dead = registry.get<FPSShooter::FpsAnimationParametersComponent>(pawn);
    if (!dead.dead || dead.idle || dead.moving || dead.jumping || dead.reloading || dead.firing)
        return fail(name, "death did not clear live animation parameters");

    const entt::entity representation = registry.create();
    auto& player_representation = registry.emplace<PlayerRepresentationComponent>(representation);
    player_representation.tracked_player = ensureEntityRef(registry, pawn);
    FPSShooter::updateAnimationParameters(registry);
    if (!registry.all_of<FPSShooter::FpsAnimationParametersComponent>(representation) ||
        !registry.get<FPSShooter::FpsAnimationParametersComponent>(representation).dead)
    {
        return fail(name, "representation did not receive its tracked pawn parameters");
    }

    const entt::entity missing_representation = registry.create();
    registry.emplace<PlayerRepresentationComponent>(missing_representation);
    registry.emplace<FPSShooter::FpsAnimationParametersComponent>(missing_representation);
    FPSShooter::updateAnimationParameters(registry);
    if (registry.all_of<FPSShooter::FpsAnimationParametersComponent>(missing_representation))
        return fail(name, "missing tracked pawn retained representation parameters");

    registry.destroy(pawn);
    FPSShooter::updateAnimationParameters(registry);
    if (!registry.valid(representation) ||
        registry.all_of<FPSShooter::FpsAnimationParametersComponent>(representation))
    {
        return fail(name, "missing or destroyed tracked pawn retained representation parameters");
    }

    return pass(name);
}

bool testPlayerMovementInputContract()
{
    const std::string name = "Player_MovementInputContract";

    InputManager input;
    InputActionSystem actions;
    for (const char* action : {"MoveForward", "MoveBack", "MoveLeft", "MoveRight", "Jump", "Use",
                               "Sprint", "Crouch", "Walk", "Drop", "Fire", "Reload", "WeaponSlot1",
                               "WeaponSlot2", "WeaponSlot3", "WeaponSlot4", "WeaponSlot5"}) {
        if (!actions.registerAction({action, ActionValueType::Button, false}))
            return fail(name, "could not register input action");
    }

    MappingContext gameplay;
    gameplay.name = "PlayerMovementTest";
    gameplay.bindings = {
        {"Jump", InputBinding::Source::Key, SDL_SCANCODE_SPACE},
        {"MoveForward", InputBinding::Source::Key, SDL_SCANCODE_W},
        {"MoveRight", InputBinding::Source::Key, SDL_SCANCODE_D},
        {"Sprint", InputBinding::Source::Key, SDL_SCANCODE_LSHIFT},
        {"Crouch", InputBinding::Source::Key, SDL_SCANCODE_LCTRL},
    };
    if (!actions.registerContext(gameplay) || !actions.pushContext("PlayerMovementTest"))
        return fail(name, "could not activate input context");

    input.update();
    for (const SDL_Scancode key : {SDL_SCANCODE_W, SDL_SCANCODE_D}) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.scancode = key;
        input.process_event(event);
    }
    actions.evaluate(input);
    input.update();
    for (const SDL_Scancode key : {SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL}) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.scancode = key;
        input.process_event(event);
    }
    actions.evaluate(input);
    const Net::InputState client_input = FPSShooter::sampleNetworkInput(
        actions, 0.25f, -0.1f, true, false, false, false);
    const uint16_t movement_buttons = Net::InputFlags::JUMP |
        Net::InputFlags::CROUCH | Net::InputFlags::SPRINT;
    if ((client_input.buttons & movement_buttons) != movement_buttons ||
        client_input.move_forward != 1.0f || client_input.move_right != 1.0f)
        return fail(name, "input sampler lost movement or jump while direction keys were held");

    input.update();
    actions.evaluate(input);
    const Net::InputState held_input = FPSShooter::sampleNetworkInput(
        actions, 0.25f, -0.1f, true, false, false, false);
    if ((held_input.buttons & movement_buttons) != movement_buttons) {
        return fail(name, "held jump or movement modifiers were lost");
    }

    const Net::MovementInput predicted = FPSShooter::toMovementInput(client_input);
    const CharacterMoveInput predicted_character = FPSShooter::toCharacterMoveInput(predicted);
    const uint16_t expected_character_buttons = CharacterMoveFlags::Jump |
        CharacterMoveFlags::Crouch | CharacterMoveFlags::Sprint;
    if (predicted_character.buttons != expected_character_buttons ||
        predicted_character.move_forward != 1.0f || predicted_character.move_right != 1.0f ||
        !approx(predicted_character.camera_yaw, client_input.camera_yaw) ||
        !approx(predicted_character.camera_pitch, client_input.camera_pitch)) {
        return fail(name, "prediction bridge did not preserve movement intent");
    }

    Net::InputSample authoritative_sample;
    authoritative_sample.tick = 37;
    authoritative_sample.move_forward = client_input.move_forward;
    authoritative_sample.move_right = client_input.move_right;
    authoritative_sample.camera_yaw = client_input.camera_yaw;
    authoritative_sample.camera_pitch = client_input.camera_pitch;
    authoritative_sample.buttons = movement_buttons;
    const CharacterMoveInput authoritative_character =
        FPSShooter::toCharacterMoveInput(FPSShooter::toMovementInput(authoritative_sample));
    if (authoritative_character.buttons != expected_character_buttons ||
        authoritative_character.move_forward != 1.0f || authoritative_character.move_right != 1.0f ||
        !approx(authoritative_character.camera_yaw, client_input.camera_yaw) ||
        !approx(authoritative_character.camera_pitch, client_input.camera_pitch)) {
        return fail(name, "authoritative bridge did not preserve movement intent");
    }

    authoritative_sample.buttons = Net::withWeaponSelect(
        movement_buttons, Net::InputFlags::WEAPON_SELECT_RESERVED);
    const Net::MovementInput sanitized = FPSShooter::toMovementInput(authoritative_sample);
    if (Net::getWeaponSelect(sanitized.buttons) != 0 ||
        (sanitized.buttons & movement_buttons) != movement_buttons) {
        return fail(name, "reserved weapon selection corrupted movement input");
    }

    if (!FPSShooter::isMovementInputAllowed(true, false) ||
        FPSShooter::isMovementInputAllowed(false, false) ||
        FPSShooter::isMovementInputAllowed(true, true)) {
        return fail(name, "dead or frozen movement gate was not neutral");
    }
    if (FPSShooter::isLocalControllerMovementEnabled(true) ||
        !FPSShooter::isLocalControllerMovementEnabled(false)) {
        return fail(name, "local controller and network prediction can both own movement");
    }

    return testPlayerPawnLifecycleContract() && testPlayerSpectatorStateContract() &&
        testPlayerLocalSpawnSwitchKillRespawnContract() &&
        testPlayerAnimationParameterContract() && pass(name);
}
