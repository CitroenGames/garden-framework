#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include "NetworkProtocol.hpp" // For InputFlags

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
        MovementState result = current;

        // 1. Build wish direction from input axes
        glm::vec3 wish_dir(input.move_right, 0.0f, input.move_forward);

        // 2. Rotate by camera orientation (matches camera.hpp camera_rot_quaternion())
        //    Camera stores rotation as radians: x=pitch, y=yaw, z=roll(0)
        //    Pitch clamped to [-1, 1] radians (~57 degrees)
        float clamped_pitch = glm::clamp(input.camera_pitch, -1.0f, 1.0f);
        glm::quat cam_rot = glm::quat(glm::vec3(clamped_pitch, input.camera_yaw, 0.0f));
        wish_dir = cam_rot * wish_dir;

        // 3. Project onto ground plane or flatten for air
        if (current.grounded)
        {
            // Project wish_dir onto ground plane: v - dot(v, n) * n
            wish_dir = wish_dir - glm::dot(wish_dir, current.ground_normal) * current.ground_normal;
        }
        else
        {
            wish_dir.y = 0.0f;
        }

        // 4. Normalize (or zero if negligible)
        if (glm::length(wish_dir) > 0.001f)
            wish_dir = glm::normalize(wish_dir);
        else
            wish_dir = glm::vec3(0.0f);

        // 5. Target horizontal velocity
        glm::vec3 target_horizontal = wish_dir * config.speed;

        bool wish_jump = (input.buttons & InputFlags::JUMP) != 0;

        if (current.grounded)
        {
            // 6a. Grounded: friction blend for responsive but smooth movement
            float ga = config.ground_friction_authority;
            result.velocity.x = target_horizontal.x * ga + current.velocity.x * (1.0f - ga);
            result.velocity.z = target_horizontal.z * ga + current.velocity.z * (1.0f - ga);

            if (wish_jump)
            {
                result.velocity.y = config.jump_force;
                result.grounded = false;
            }
            else
            {
                result.velocity.y = 0.0f; // Stick to surface
            }
        }
        else
        {
            // 6b. Airborne: reduced authority for air control
            float aa = config.air_control_authority;
            result.velocity.x = target_horizontal.x * aa + current.velocity.x * (1.0f - aa);
            result.velocity.z = target_horizontal.z * aa + current.velocity.z * (1.0f - aa);

            // Apply gravity
            result.velocity.y -= config.gravity_magnitude * config.fixed_delta;
        }

        // 7. Integrate position
        result.position += result.velocity * config.fixed_delta;

        return result;
    }
} // namespace SharedMovement
