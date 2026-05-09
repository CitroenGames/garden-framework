#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include "Character/CharacterController.hpp"
#include "NetworkProtocol.hpp" // For InputFlags

namespace Net {

// Input command for one simulation tick
struct MovementInput
{
    float move_forward = 0.0f;  // -1.0 to 1.0
    float move_right = 0.0f;    // -1.0 to 1.0
    float camera_yaw = 0.0f;    // radians
    float camera_pitch = 0.0f;  // radians
    uint8_t buttons = 0;        // InputFlags bitfield
};

// Player movement state at a point in time
struct MovementState
{
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
    int water_level = CharacterWaterLevel::None;
};

// Tuning parameters for movement simulation
struct MovementConfig
{
    float speed = 10.0f;
    float jump_force = 5.0f;
    float fixed_delta = 1.0f / 60.0f;
    float gravity_magnitude = 9.81f;
    float ground_friction_authority = 0.8f;
    float air_control_authority = 0.3f;
    float ground_acceleration = 5.5f;
    float air_acceleration = 12.0f;
    float friction = 5.2f;
    float stop_speed_ratio = 0.25f;
    float air_wish_speed_cap_ratio = 30.0f / 320.0f;
    float surface_friction = 1.0f;
    float max_velocity = 100.0f;
    int water_level = CharacterWaterLevel::None;
    float water_speed_scale = 0.8f;
    float water_acceleration = 5.5f;
    float water_friction = 5.2f;
    float water_sink_speed = 1.75f;
};

namespace SharedMovement
{
    // Run one tick of deterministic player movement.
    // Both client (for prediction) and server (for authoritative sim) must call
    // this with identical inputs to produce identical results.
    inline MovementState simulate(const MovementInput& input,
                                  const MovementState& current,
                                  const MovementConfig& config)
    {
        CharacterMoveInput character_input;
        character_input.move_forward = input.move_forward;
        character_input.move_right = input.move_right;
        character_input.camera_yaw = input.camera_yaw;
        character_input.camera_pitch = input.camera_pitch;
        if ((input.buttons & InputFlags::JUMP) != 0)
            character_input.buttons |= CharacterMoveFlags::Jump;

        CharacterControllerState character_state;
        character_state.position = current.position;
        character_state.velocity = current.velocity;
        character_state.grounded = current.grounded;
        character_state.ground_normal = current.ground_normal;
        character_state.water_level = current.water_level;

        CharacterController::MovementTuning tuning;
        tuning.max_speed = config.speed;
        tuning.jump_velocity = config.jump_force;
        tuning.gravity = glm::vec3(0.0f, -std::max(config.gravity_magnitude, 0.0f), 0.0f);
        tuning.fixed_delta = config.fixed_delta;
        tuning.ground_acceleration = config.ground_acceleration;
        tuning.air_acceleration = config.air_acceleration;
        tuning.friction = config.friction;
        tuning.stop_speed = std::max(config.speed, 0.0f) * std::max(config.stop_speed_ratio, 0.0f);
        tuning.air_wish_speed_cap = std::max(config.speed, 0.0f) * std::max(config.air_wish_speed_cap_ratio, 0.0f);
        tuning.surface_friction = config.surface_friction;
        tuning.max_velocity = config.max_velocity;
        tuning.water_level = config.water_level;
        tuning.water_speed_scale = config.water_speed_scale;
        tuning.water_acceleration = config.water_acceleration;
        tuning.water_friction = config.water_friction;
        tuning.water_sink_speed = config.water_sink_speed;

        const CharacterControllerState simulated =
            CharacterController::simulateSourceMovement(character_input, character_state, tuning);

        MovementState result;
        result.position = simulated.position;
        result.velocity = simulated.velocity;
        result.grounded = simulated.grounded;
        result.ground_normal = simulated.ground_normal;
        result.water_level = simulated.water_level;
        return result;
    }
} // namespace SharedMovement

} // namespace Net
