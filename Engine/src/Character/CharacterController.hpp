#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace CharacterMoveFlags
{
    constexpr uint8_t Jump = 1 << 0;
}

namespace CharacterWaterLevel
{
    constexpr int None = 0;
    constexpr int Feet = 1;
    constexpr int Waist = 2;
    constexpr int Eyes = 3;
}

struct CharacterMoveInput
{
    float move_forward = 0.0f;
    float move_right = 0.0f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    uint8_t buttons = 0;
};

struct CharacterControllerState
{
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 ground_velocity = glm::vec3(0.0f);
    int water_level = CharacterWaterLevel::None;
};

namespace CharacterController
{
    struct MovementTuning
    {
        float max_speed = 10.0f;
        float jump_velocity = 5.0f;
        glm::vec3 gravity = glm::vec3(0.0f, -9.81f, 0.0f);
        float gravity_scale = 1.0f;
        float fixed_delta = 1.0f / 60.0f;
        float ground_acceleration = 5.5f;
        float air_acceleration = 12.0f;
        float friction = 5.2f;
        float stop_speed = 2.5f;
        float air_wish_speed_cap = 0.9375f;
        float surface_friction = 1.0f;
        float max_velocity = 100.0f;
        int water_level = CharacterWaterLevel::None;
        float water_speed_scale = 0.8f;
        float water_acceleration = 5.5f;
        float water_friction = 5.2f;
        float water_sink_speed = 1.75f;
    };

    struct WishMove
    {
        glm::vec3 direction = glm::vec3(0.0f);
        float speed = 0.0f;
    };

    inline bool wantsJump(const CharacterMoveInput& input)
    {
        return (input.buttons & CharacterMoveFlags::Jump) != 0;
    }

    inline float horizontalLength(const glm::vec3& v)
    {
        return std::sqrt(v.x * v.x + v.z * v.z);
    }

    inline glm::vec3 clampVectorLength(const glm::vec3& v, float max_length)
    {
        const float len = glm::length(v);
        if (max_length > 0.0f && len > max_length)
            return v * (max_length / len);
        return v;
    }

    inline WishMove buildWishMove(const CharacterMoveInput& input, float max_speed)
    {
        WishMove wish;
        const float clamped_forward = std::clamp(input.move_forward, -1.0f, 1.0f);
        const float clamped_right = std::clamp(input.move_right, -1.0f, 1.0f);
        const float clamped_speed = std::max(max_speed, 0.0f);

        const glm::quat yaw_rotation = glm::quat(glm::vec3(0.0f, input.camera_yaw, 0.0f));
        const glm::vec3 forward = yaw_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 right = yaw_rotation * glm::vec3(1.0f, 0.0f, 0.0f);

        glm::vec3 wish_velocity =
            forward * (clamped_forward * clamped_speed) +
            right * (clamped_right * clamped_speed);
        wish_velocity.y = 0.0f;

        wish.speed = horizontalLength(wish_velocity);
        if (wish.speed > clamped_speed && wish.speed > 0.0f)
        {
            wish_velocity *= clamped_speed / wish.speed;
            wish.speed = clamped_speed;
        }

        if (wish.speed > 0.001f)
            wish.direction = wish_velocity / wish.speed;

        return wish;
    }

    inline WishMove buildWaterWishMove(const CharacterMoveInput& input,
                                       const MovementTuning& tuning)
    {
        WishMove wish;
        const float clamped_forward = std::clamp(input.move_forward, -1.0f, 1.0f);
        const float clamped_right = std::clamp(input.move_right, -1.0f, 1.0f);
        const float clamped_speed = std::max(tuning.max_speed, 0.0f);

        const glm::vec3 view_rotation(
            std::clamp(input.camera_pitch, -1.5f, 1.5f),
            input.camera_yaw,
            0.0f);
        const glm::quat look_rotation = glm::quat(view_rotation);
        const glm::quat yaw_rotation = glm::quat(glm::vec3(0.0f, input.camera_yaw, 0.0f));

        const glm::vec3 forward = look_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 right = yaw_rotation * glm::vec3(1.0f, 0.0f, 0.0f);

        glm::vec3 wish_velocity =
            forward * (clamped_forward * clamped_speed) +
            right * (clamped_right * clamped_speed);

        if (wantsJump(input))
            wish_velocity.y += clamped_speed;
        else if (std::abs(clamped_forward) < 0.001f && std::abs(clamped_right) < 0.001f)
            wish_velocity.y -= std::max(tuning.water_sink_speed, 0.0f);

        wish.speed = glm::length(wish_velocity);
        if (wish.speed > clamped_speed && wish.speed > 0.0f)
        {
            wish_velocity *= clamped_speed / wish.speed;
            wish.speed = clamped_speed;
        }

        wish.speed *= std::clamp(tuning.water_speed_scale, 0.0f, 1.0f);
        if (wish.speed > 0.001f)
            wish.direction = wish_velocity / glm::length(wish_velocity);

        return wish;
    }

    inline glm::vec3 applyFriction(glm::vec3 velocity, const MovementTuning& tuning)
    {
        velocity.y = 0.0f;
        const float speed = horizontalLength(velocity);
        if (speed < 0.1f)
            return velocity;

        const float control = speed < tuning.stop_speed ? tuning.stop_speed : speed;
        const float drop = control * std::max(tuning.friction, 0.0f) *
            std::max(tuning.surface_friction, 0.0f) * tuning.fixed_delta;
        const float new_speed = std::max(speed - drop, 0.0f);
        if (new_speed == speed)
            return velocity;

        return velocity * (new_speed / speed);
    }

    inline glm::vec3 applyWaterFriction(glm::vec3 velocity, const MovementTuning& tuning)
    {
        const float speed = glm::length(velocity);
        if (speed < 0.1f)
            return velocity;

        const float drop = speed * std::max(tuning.water_friction, 0.0f) *
            std::max(tuning.surface_friction, 0.0f) * tuning.fixed_delta;
        const float new_speed = std::max(speed - drop, 0.0f);
        if (new_speed == speed)
            return velocity;

        return velocity * (new_speed / speed);
    }

    inline glm::vec3 accelerate(glm::vec3 velocity,
                                const glm::vec3& wish_dir,
                                float wish_speed,
                                float acceleration,
                                const MovementTuning& tuning)
    {
        if (wish_speed <= 0.0f)
            return velocity;

        const float current_speed = glm::dot(velocity, wish_dir);
        const float add_speed = wish_speed - current_speed;
        if (add_speed <= 0.0f)
            return velocity;

        float accel_speed = std::max(acceleration, 0.0f) * wish_speed *
            tuning.fixed_delta * std::max(tuning.surface_friction, 0.0f);
        if (accel_speed > add_speed)
            accel_speed = add_speed;

        return velocity + wish_dir * accel_speed;
    }

    inline glm::vec3 airAccelerate(glm::vec3 velocity,
                                   const glm::vec3& wish_dir,
                                   float wish_speed,
                                   const MovementTuning& tuning)
    {
        const float capped_wish_speed = std::min(wish_speed, std::max(tuning.air_wish_speed_cap, 0.0f));
        if (capped_wish_speed <= 0.0f)
            return velocity;

        const float current_speed = glm::dot(velocity, wish_dir);
        const float add_speed = capped_wish_speed - current_speed;
        if (add_speed <= 0.0f)
            return velocity;

        float accel_speed = std::max(tuning.air_acceleration, 0.0f) * wish_speed *
            tuning.fixed_delta * std::max(tuning.surface_friction, 0.0f);
        if (accel_speed > add_speed)
            accel_speed = add_speed;

        return velocity + wish_dir * accel_speed;
    }

    inline CharacterControllerState simulateWaterMovement(const CharacterMoveInput& input,
                                                          const CharacterControllerState& current,
                                                          const MovementTuning& tuning)
    {
        CharacterControllerState result = current;
        result.grounded = false;
        result.ground_velocity = glm::vec3(0.0f);
        result.water_level = tuning.water_level;

        const WishMove wish = buildWaterWishMove(input, tuning);
        glm::vec3 new_velocity = applyWaterFriction(current.velocity, tuning);
        new_velocity = accelerate(new_velocity, wish.direction, wish.speed, tuning.water_acceleration, tuning);
        new_velocity = clampVectorLength(new_velocity, tuning.max_velocity);

        result.velocity = new_velocity;
        result.position += result.velocity * tuning.fixed_delta;
        return result;
    }

    inline CharacterControllerState simulateSourceMovement(const CharacterMoveInput& input,
                                                           const CharacterControllerState& current,
                                                           MovementTuning tuning)
    {
        tuning.max_speed = std::max(tuning.max_speed, 0.0f);
        tuning.jump_velocity = std::max(tuning.jump_velocity, 0.0f);
        tuning.fixed_delta = std::max(tuning.fixed_delta, 0.0f);
        tuning.stop_speed = std::max(tuning.stop_speed, 0.0f);
        tuning.air_wish_speed_cap = std::max(tuning.air_wish_speed_cap, 0.0f);
        tuning.water_level = std::clamp(tuning.water_level, CharacterWaterLevel::None, CharacterWaterLevel::Eyes);

        if (tuning.water_level >= CharacterWaterLevel::Waist)
            return simulateWaterMovement(input, current, tuning);

        CharacterControllerState result = current;
        const WishMove wish = buildWishMove(input, tuning.max_speed);
        const bool jump = wantsJump(input) && current.grounded;

        glm::vec3 new_velocity = current.velocity;
        if (current.grounded)
        {
            glm::vec3 relative_velocity = current.velocity - current.ground_velocity;
            relative_velocity.y = 0.0f;

            if (!jump)
                relative_velocity = applyFriction(relative_velocity, tuning);

            relative_velocity = accelerate(
                relative_velocity, wish.direction, wish.speed, tuning.ground_acceleration, tuning);

            if (!jump)
                relative_velocity = clampVectorLength(relative_velocity, tuning.max_speed);

            new_velocity = current.ground_velocity;
            new_velocity.x += relative_velocity.x;
            new_velocity.z += relative_velocity.z;

            if (jump)
            {
                new_velocity.y = current.velocity.y + tuning.jump_velocity;
                result.grounded = false;
            }
            else
            {
                new_velocity.y = current.ground_velocity.y;
            }
        }
        else
        {
            glm::vec3 horizontal_velocity(current.velocity.x, 0.0f, current.velocity.z);
            horizontal_velocity = airAccelerate(horizontal_velocity, wish.direction, wish.speed, tuning);

            new_velocity.x = horizontal_velocity.x;
            new_velocity.z = horizontal_velocity.z;
        }

        new_velocity += tuning.gravity * tuning.gravity_scale * tuning.fixed_delta;
        new_velocity = clampVectorLength(new_velocity, tuning.max_velocity);

        result.velocity = new_velocity;
        result.position += result.velocity * tuning.fixed_delta;
        return result;
    }
}
