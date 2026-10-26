#pragma once

#include "Character/CharacterController.hpp"
#include "Components/Components.hpp"

#include <algorithm>
#include <cmath>

namespace FpsCharacter
{
    struct Config
    {
        float walk_speed = 6.0f;
        float sprint_speed = 9.0f;
        float crouch_speed = 2.7f;
        float jump_height = 1.1f;
        float gravity = 9.81f;
        float ground_acceleration = 10.0f;
        float air_acceleration = 12.0f;
        float friction = 8.0f;
        float capsule_half_height = 0.7f;
        float capsule_radius = 0.35f;
        float eye_height = 1.62f;
        float coyote_time = 0.12f;
        float jump_buffer_time = 0.12f;

        float jumpVelocity() const
        {
            return CharacterController::jumpVelocityForHeight(jump_height, gravity);
        }
    };

    inline Config tactical()
    {
        Config c;
        c.walk_speed = 4.5f;
        c.sprint_speed = 6.5f;
        c.crouch_speed = 2.0f;
        c.jump_height = 0.9f;
        c.ground_acceleration = 10.0f;
        c.air_acceleration = 6.0f;
        c.friction = 9.0f;
        return c;
    }

    inline Config arcade()
    {
        Config c;
        c.walk_speed = 7.0f;
        c.sprint_speed = 10.5f;
        c.crouch_speed = 3.2f;
        c.jump_height = 1.3f;
        c.ground_acceleration = 14.0f;
        c.air_acceleration = 14.0f;
        c.friction = 7.0f;
        return c;
    }

    inline Config sourceLike()
    {
        Config c;
        c.walk_speed = 6.0f;
        c.sprint_speed = 9.0f;
        c.crouch_speed = 2.7f;
        c.jump_height = 1.1f;
        c.ground_acceleration = 5.5f;
        c.air_acceleration = 12.0f;
        c.friction = 5.2f;
        return c;
    }

    inline CharacterControllerComponent toComponent(const Config& config)
    {
        CharacterControllerComponent component;
        component.move_speed = std::max(config.walk_speed, 0.0f);
        component.jump_velocity = config.jumpVelocity();
        component.ground_acceleration = std::max(config.ground_acceleration, 0.0f);
        component.air_acceleration = std::max(config.air_acceleration, 0.0f);
        component.friction = std::max(config.friction, 0.0f);
        component.capsule_half_height = std::max(config.capsule_half_height, 0.01f);
        component.capsule_radius = std::max(config.capsule_radius, 0.01f);
        const float walk = std::max(config.walk_speed, 0.001f);
        component.sprint_speed_scale = std::max(config.sprint_speed, 0.0f) / walk;
        component.crouch_speed_scale = std::max(config.crouch_speed, 0.0f) / walk;
        component.coyote_time = std::max(config.coyote_time, 0.0f);
        component.jump_buffer_time = std::max(config.jump_buffer_time, 0.0f);
        return component;
    }

    inline float eyeHeight(const Config& config)
    {
        return config.eye_height;
    }

    inline CharacterMoveInput makeInput(float forward, float right, float yaw, float pitch,
                                        bool jump, bool sprint, bool crouch)
    {
        CharacterMoveInput input;
        input.move_forward = std::clamp(forward, -1.0f, 1.0f);
        input.move_right = std::clamp(right, -1.0f, 1.0f);
        input.camera_yaw = yaw;
        input.camera_pitch = pitch;
        if (jump)
            input.buttons |= CharacterMoveFlags::Jump;
        if (sprint)
            input.buttons |= CharacterMoveFlags::Sprint;
        if (crouch)
            input.buttons |= CharacterMoveFlags::Crouch;
        return input;
    }
}
