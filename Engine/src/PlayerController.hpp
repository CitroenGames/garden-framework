#pragma once

#include "EngineExport.h"
#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "InputManager.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <entt/entt.hpp>
#include <memory>

enum class PossessedEntityType
{
    Player,
    Freecam
};

class ENGINE_API PlayerController
{
private:
    entt::entity player_entity;
    entt::entity freecam_entity;
    std::shared_ptr<InputManager> input_manager;
    PossessedEntityType currently_possessed;
    bool freecam_mode_enabled;
    bool movement_enabled = true; // When false, updatePlayer() only does camera follow (networked mode)
    world* game_world;

public:
    PlayerController(std::shared_ptr<InputManager> input_mgr, world* w)
        : player_entity(entt::null),
          freecam_entity(entt::null),
          input_manager(input_mgr),
          currently_possessed(PossessedEntityType::Player),
          freecam_mode_enabled(false),
          game_world(w)
    {
        setup_input_bindings();
    }

    void setup_input_bindings()
    {
        if (!input_manager) return;

        input_manager->bind_action("ToggleFreecam", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                toggleFreecamMode();
            }
        });

        input_manager->bind_action("Quit", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                exit(0);
            }
        });
    }

    void setPossessedPlayer(entt::entity player)
    {
        player_entity = player;
    }

    void setPossessedFreecam(entt::entity freecam)
    {
        freecam_entity = freecam;
    }

    void toggleFreecamMode()
    {
        if (!game_world->registry.valid(player_entity) || !game_world->registry.valid(freecam_entity))
            return;

        freecam_mode_enabled = !freecam_mode_enabled;

        if (freecam_mode_enabled)
        {
            currently_possessed = PossessedEntityType::Freecam;

            auto& p_trans = game_world->registry.get<TransformComponent>(player_entity);
            auto& f_trans = game_world->registry.get<TransformComponent>(freecam_entity);
            f_trans.position = p_trans.position;

            printf("Switched to freecam mode\n");
        }
        else
        {
            currently_possessed = PossessedEntityType::Player;
            printf("Switched to player mode\n");
        }
    }

    void handleMouseMotion(float yrel, float xrel)
    {
        if (!game_world) return;

        camera& active_cam = game_world->world_camera;
        float sensitivity = 1.0f;

        if (currently_possessed == PossessedEntityType::Player && game_world->registry.valid(player_entity))
        {
            auto& pc = game_world->registry.get<PlayerComponent>(player_entity);
            sensitivity = pc.mouse_sensitivity;
        }
        else if (currently_possessed == PossessedEntityType::Freecam && game_world->registry.valid(freecam_entity))
        {
            auto& fc = game_world->registry.get<FreecamComponent>(freecam_entity);
            sensitivity = fc.mouse_sensitivity;
        }

        float effective_sensitivity_x = input_manager ? input_manager->Sensitivity_X * sensitivity : sensitivity;
        float effective_sensitivity_y = input_manager ? input_manager->Sensitivity_Y * sensitivity : sensitivity;

        active_cam.rotation.x += yrel / 1000.0f * effective_sensitivity_y;
        active_cam.rotation.y += -xrel / 1000.0f * effective_sensitivity_x;

        if (active_cam.rotation.x > 1.5f) active_cam.rotation.x = 1.5f;
        if (active_cam.rotation.x < -1.5f) active_cam.rotation.x = -1.5f;
    }

    void update(float delta)
    {
        if (!game_world) return;

        if (currently_possessed == PossessedEntityType::Player)
        {
            updatePlayer(delta);
        }
        else if (currently_possessed == PossessedEntityType::Freecam)
        {
            updateFreecam(delta);
        }
    }

private:
    void followCamera(const glm::vec3& position, float delta)
    {
        float camera_speed = 10.0f;
        float t = 1.0f - std::exp(-camera_speed * delta);
        game_world->world_camera.position = glm::mix(game_world->world_camera.position, position, t);
    }

    CharacterMoveInput collectPlayerMoveInput() const
    {
        CharacterMoveInput input;
        if (!input_manager)
            return input;

        if (input_manager->is_key_held(SDL_SCANCODE_W)) input.move_forward += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) input.move_forward -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) input.move_right -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) input.move_right += 1.0f;
        if (input_manager->is_key_pressed(SDL_SCANCODE_SPACE)) input.buttons |= CharacterMoveFlags::Jump;
        input.camera_yaw = game_world->world_camera.rotation.y;
        input.camera_pitch = game_world->world_camera.rotation.x;
        return input;
    }

    void updatePlayer(float delta)
    {
        if (!game_world->registry.valid(player_entity)) return;

        auto& trans = game_world->registry.get<TransformComponent>(player_entity);

        if (!movement_enabled)
        {
            followCamera(trans.position, delta);
            return;
        }

        if (game_world->registry.all_of<CharacterControllerComponent>(player_entity))
        {
            auto& controller = game_world->registry.get<CharacterControllerComponent>(player_entity);
            bool input_enabled = controller.input_enabled;
            if (game_world->registry.all_of<PlayerComponent>(player_entity))
                input_enabled = input_enabled && game_world->registry.get<PlayerComponent>(player_entity).input_enabled;
            if (!input_enabled || !input_manager)
                return;

            CharacterMoveInput input = collectPlayerMoveInput();
            CharacterControllerState state =
                game_world->simulate_character_controller(player_entity, input, game_world->fixed_delta);

            followCamera(state.position, delta);
            return;
        }

        auto& pc = game_world->registry.get<PlayerComponent>(player_entity);
        auto& rb = game_world->registry.get<RigidBodyComponent>(player_entity);
        if (!pc.input_enabled || !input_manager)
            return;

        CharacterMoveInput input = collectPlayerMoveInput();

        CharacterControllerState current;
        current.position = trans.position;
        current.velocity = rb.velocity;
        current.grounded = pc.grounded;
        current.ground_normal = pc.ground_normal;

        CharacterController::MovementTuning tuning;
        tuning.max_speed = pc.speed;
        tuning.jump_velocity = pc.jump_force;
        tuning.gravity = game_world->getGravity() * 9.81f;
        tuning.fixed_delta = delta;
        tuning.stop_speed = std::max(pc.speed, 0.0f) * 0.25f;
        tuning.air_wish_speed_cap = std::max(pc.speed, 0.0f) * (30.0f / 320.0f);

        const CharacterControllerState simulated =
            CharacterController::simulateSourceMovement(input, current, tuning);
        rb.velocity = simulated.velocity;
        pc.grounded = simulated.grounded;
        pc.ground_normal = simulated.ground_normal;

        const glm::vec3 wish_dir = CharacterController::buildWishMove(input, tuning.max_speed).direction;
        LOG_ENGINE_TRACE("[PlayerCtrl] wish=({},{},{}) vel=({},{},{}) grounded={}",
            wish_dir.x, wish_dir.y, wish_dir.z,
            rb.velocity.x, rb.velocity.y, rb.velocity.z, pc.grounded);

        followCamera(trans.position, delta);
    }

    void updateFreecam(float delta)
    {
        if (!game_world->registry.valid(freecam_entity)) return;

        auto& fc = game_world->registry.get<FreecamComponent>(freecam_entity);
        auto& trans = game_world->registry.get<TransformComponent>(freecam_entity);

        if (!fc.input_enabled || !input_manager) return;

        glm::vec3 local_movement = glm::vec3(0, 0, 0);

        if (input_manager->is_key_held(SDL_SCANCODE_W)) local_movement.z += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) local_movement.z -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) local_movement.x += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) local_movement.x -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) local_movement.y += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_LSHIFT)) local_movement.y -= 1.0f;

        if (glm::length(local_movement) > 0)
            local_movement = glm::normalize(local_movement);

        float current_speed = input_manager->is_key_held(SDL_SCANCODE_LSHIFT)
            ? fc.fast_movement_speed
            : fc.movement_speed;

        glm::vec3 world_movement = game_world->world_camera.camera_rot_quaternion() * local_movement;
        game_world->world_camera.position += world_movement * current_speed * delta;
        trans.position = game_world->world_camera.position;
    }

public:
    camera& getActiveCamera()
    {
        return game_world->world_camera;
    }

    bool isFreecamMode() const { return freecam_mode_enabled; }

    void setMovementEnabled(bool enabled) { movement_enabled = enabled; }
    bool isMovementEnabled() const { return movement_enabled; }
};
