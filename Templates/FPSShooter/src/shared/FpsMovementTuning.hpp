#pragma once

#include "Components/Components.hpp"

namespace FPSShooter
{
    // Template movement policy in metres and seconds. The jump impulse yields
    // approximately 0.46 m of rise under the default 9.81 m/s² gravity.
    inline constexpr float kMoveSpeed = 4.5f;
    inline constexpr float kJumpVelocity = 3.0f;

    inline void applyMovementTuning(PlayerComponent& player, CharacterControllerComponent& controller)
    {
        player.speed = kMoveSpeed;
        player.jump_force = kJumpVelocity;
        controller.move_speed = kMoveSpeed;
        controller.jump_velocity = kJumpVelocity;
    }
}
