#pragma once

#include "Character/CharacterController.hpp"
#include "Network/NetworkInput.hpp"
#include "Network/NetworkInputState.hpp"
#include "Network/SharedMovement.hpp"

namespace FPSShooter
{
    // The server remains authoritative. The client uses the replicated freeze
    // state only to avoid predicting a movement command that authority will
    // discard; before that state arrives, movement remains enabled.
    inline bool isMovementInputAllowed(bool alive, bool movement_locked)
    {
        return alive && !movement_locked;
    }

    // PlayerController is local input/camera glue. Network prediction owns
    // local pawn simulation while a client is connected so intent is applied
    // exactly once for each command.
    inline bool isLocalControllerMovementEnabled(bool network_prediction_active)
    {
        return !network_prediction_active;
    }

    // The protocol's only invalid button encoding is the reserved weapon slot.
    // Keep its normalization shared by local prediction and direct template
    // input producers such as bots.
    inline uint16_t sanitizeInputButtons(uint16_t buttons)
    {
        return Net::sanitizeWeaponSelect(buttons, nullptr);
    }

    inline Net::MovementInput toMovementInput(const Net::InputState& input)
    {
        Net::MovementInput out;
        out.move_forward = input.move_forward;
        out.move_right = input.move_right;
        out.camera_yaw = input.camera_yaw;
        out.camera_pitch = input.camera_pitch;
        out.buttons = sanitizeInputButtons(input.buttons);
        return out;
    }

    inline Net::MovementInput toMovementInput(const Net::InputSample& input)
    {
        Net::MovementInput out;
        out.move_forward = input.move_forward;
        out.move_right = input.move_right;
        out.camera_yaw = input.camera_yaw;
        out.camera_pitch = input.camera_pitch;
        out.buttons = sanitizeInputButtons(input.buttons);
        out.view_server_tick = input.view_server_tick;
        return out;
    }

    inline CharacterMoveInput toCharacterMoveInput(const Net::MovementInput& input)
    {
        CharacterMoveInput out;
        out.move_forward = input.move_forward;
        out.move_right = input.move_right;
        out.camera_yaw = input.camera_yaw;
        out.camera_pitch = input.camera_pitch;
        if ((input.buttons & Net::InputFlags::JUMP) != 0)
            out.buttons |= CharacterMoveFlags::Jump;
        if ((input.buttons & Net::InputFlags::CROUCH) != 0)
            out.buttons |= CharacterMoveFlags::Crouch;
        if ((input.buttons & Net::InputFlags::SPRINT) != 0)
            out.buttons |= CharacterMoveFlags::Sprint;
        return out;
    }
}
