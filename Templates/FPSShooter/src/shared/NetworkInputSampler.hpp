#pragma once

#include "InputActionSystem.hpp"
#include "Network/NetworkInputState.hpp"
#include "Network/NetworkProtocol.hpp"

namespace FPSShooter
{
    // Converts the game module's named actions into the existing network input
    // contract. Directional button actions intentionally keep opposing inputs
    // independently visible, matching the prior key polling behavior.
    inline Net::InputState sampleNetworkInput(
        const InputActionSystem& actions,
        float camera_yaw,
        float camera_pitch,
        bool local_alive,
        bool ui_modal,
        bool can_fire,
        bool can_reload)
    {
        Net::InputState state{};
        state.camera_yaw = camera_yaw;
        state.camera_pitch = camera_pitch;

        if (!local_alive || ui_modal)
            return state;

        if (actions.get("MoveForward").held) state.buttons |= Net::InputFlags::MOVE_FORWARD;
        if (actions.get("MoveBack").held) state.buttons |= Net::InputFlags::MOVE_BACK;
        if (actions.get("MoveLeft").held) state.buttons |= Net::InputFlags::MOVE_LEFT;
        if (actions.get("MoveRight").held) state.buttons |= Net::InputFlags::MOVE_RIGHT;
        // The host samples once per rendered update and the network manager
        // latches one-shot bits until its 60 Hz send. Preserve that contract:
        // several presses within one update still produce one command flag.
        if (actions.wasPressed("Jump")) state.buttons |= Net::InputFlags::JUMP;
        if (actions.get("Use").held) state.buttons |= Net::InputFlags::USE;
        if (actions.get("Sprint").held) state.buttons |= Net::InputFlags::SPRINT;
        if (actions.get("Crouch").held) state.buttons |= Net::InputFlags::CROUCH;
        if (actions.get("Walk").held) state.buttons |= Net::InputFlags::WALK;
        if (actions.wasPressed("Drop")) state.buttons |= Net::InputFlags::DROP;
        if (can_fire && actions.get("Fire").held) state.buttons |= Net::InputFlags::ATTACK;
        if (can_reload && actions.wasPressed("Reload")) state.buttons |= Net::InputFlags::RELOAD;

        // Keep the existing precedence when several weapon keys arrive in the
        // same rendered frame: the highest checked slot wins.
        if (actions.wasPressed("WeaponSlot1")) state.buttons = Net::withWeaponSelect(state.buttons, 1);
        if (actions.wasPressed("WeaponSlot2")) state.buttons = Net::withWeaponSelect(state.buttons, 2);
        if (actions.wasPressed("WeaponSlot3")) state.buttons = Net::withWeaponSelect(state.buttons, 3);
        if (actions.wasPressed("WeaponSlot4")) state.buttons = Net::withWeaponSelect(state.buttons, 4);
        if (actions.wasPressed("WeaponSlot5")) state.buttons = Net::withWeaponSelect(state.buttons, 5);

        if (state.buttons & Net::InputFlags::MOVE_FORWARD) state.move_forward += 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_BACK) state.move_forward -= 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_RIGHT) state.move_right += 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_LEFT) state.move_right -= 1.0f;
        return state;
    }
}
