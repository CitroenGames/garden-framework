#pragma once

#include "InputActionSystem.hpp"
#include "InputManager.hpp"
#include "shared/MovementInputPolicy.hpp"
#include "shared/NetworkInputSampler.hpp"
#include "shared/PlayerLifecycle.hpp"
#include "shared/FpsAnimationPresentation.hpp"

#include <string>

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
        {"Sprint", InputBinding::Source::Key, SDL_SCANCODE_LSHIFT},
        {"Crouch", InputBinding::Source::Key, SDL_SCANCODE_LCTRL},
    };
    if (!actions.registerContext(gameplay) || !actions.pushContext("PlayerMovementTest"))
        return fail(name, "could not activate input context");

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
    if ((client_input.buttons & movement_buttons) != movement_buttons)
        return fail(name, "input sampler lost sprint, crouch, or jump");

    input.update();
    actions.evaluate(input);
    const Net::InputState held_input = FPSShooter::sampleNetworkInput(
        actions, 0.25f, -0.1f, true, false, false, false);
    if ((held_input.buttons & Net::InputFlags::JUMP) != 0 ||
        (held_input.buttons & (Net::InputFlags::CROUCH | Net::InputFlags::SPRINT)) !=
            (Net::InputFlags::CROUCH | Net::InputFlags::SPRINT)) {
        return fail(name, "jump was not edge-sampled or held movement modifiers were lost");
    }

    const Net::MovementInput predicted = FPSShooter::toMovementInput(client_input);
    const CharacterMoveInput predicted_character = FPSShooter::toCharacterMoveInput(predicted);
    const uint16_t expected_character_buttons = CharacterMoveFlags::Jump |
        CharacterMoveFlags::Crouch | CharacterMoveFlags::Sprint;
    if (predicted_character.buttons != expected_character_buttons ||
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
        testPlayerAnimationParameterContract() && pass(name);
}
