#pragma once

#include "InputActionSystem.hpp"
#include "Network/NetworkInputState.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Tick/TickSystem.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace FPSShooter
{
    // Edge actions are sampled from the render-facing action layer, then
    // consumed by the fixed command cadence. Keeping this state outside the
    // ECS pawn is deliberate: it belongs to the local input device/host and
    // must survive a zero-step render frame without becoming replicated game
    // state.
    struct NetworkInputEdges
    {
        bool jump = false;
        bool drop = false;
        bool reload = false;
        uint8_t weapon_select = 0;
    };

    class FixedNetworkInputCadence
    {
    public:
        explicit FixedNetworkInputCadence(float fixed_delta = 1.0f / 60.0f)
            : ticks(fixed_delta)
        {
        }

        void setFixedDelta(float fixed_delta)
        {
            ticks.setFixedDelta(fixed_delta);
        }

        void reset()
        {
            ticks.reset();
            jump_presses = 0;
            drop_presses = 0;
            reload_presses = 0;
            weapon_select_presses.fill(0);
        }

        // Capture after InputActionSystem::evaluate exactly once per rendered
        // frame. Press counts, rather than wasPressed(), preserve a tap that
        // arrives on a zero-step frame and distribute repeated edges across
        // later fixed command ticks.
        void captureFrameEdges(const InputActionSystem& actions)
        {
            add(jump_presses, actions.get("Jump").press_count);
            add(drop_presses, actions.get("Drop").press_count);
            add(reload_presses, actions.get("Reload").press_count);
            for (uint8_t slot = 1; slot <= 5; ++slot)
            {
                const char* action = weaponAction(slot);
                add(weapon_select_presses[slot], actions.get(action).press_count);
            }
        }

        uint32_t consumeFrame(float frame_delta)
        {
            return ticks.consume(frame_delta);
        }

        float fixedDelta() const { return ticks.getFixedDelta(); }

        static const char* weaponAction(uint8_t slot)
        {
            static constexpr std::array<const char*, 6> actions = {
                "", "WeaponSlot1", "WeaponSlot2", "WeaponSlot3", "WeaponSlot4", "WeaponSlot5"
            };
            return slot < actions.size() ? actions[slot] : actions[0];
        }

        // Call once for every consumed fixed step. Each edge becomes one bit
        // on exactly one command; held actions remain sampled by the caller.
        NetworkInputEdges consumeEdgesForTick()
        {
            NetworkInputEdges edges;
            edges.jump = take(jump_presses);
            edges.drop = take(drop_presses);
            edges.reload = take(reload_presses);

            // Preserve the existing sampler's deterministic slot precedence
            // for simultaneous selections while still consuming at most one
            // value-field selection per command tick.
            for (uint8_t slot = 1; slot <= 5; ++slot)
            {
                if (take(weapon_select_presses[slot]))
                    edges.weapon_select = slot;
            }
            return edges;
        }

    private:
        static void add(uint32_t& destination, uint32_t value)
        {
            const uint32_t maximum = (std::numeric_limits<uint32_t>::max)();
            destination = value > maximum - destination
                ? maximum
                : destination + value;
        }

        static bool take(uint32_t& source)
        {
            if (source == 0)
                return false;
            --source;
            return true;
        }

        Tick::FixedTickAccumulator ticks;
        uint32_t jump_presses = 0;
        uint32_t drop_presses = 0;
        uint32_t reload_presses = 0;
        std::array<uint32_t, 6> weapon_select_presses{};
    };

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
        bool can_reload,
        const NetworkInputEdges& edges)
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
        // Preserve a held jump for the controller while also retaining taps
        // captured on render frames with no fixed command step.
        if (edges.jump || actions.get("Jump").held) state.buttons |= Net::InputFlags::JUMP;
        if (actions.get("Use").held) state.buttons |= Net::InputFlags::USE;
        if (actions.get("Sprint").held) state.buttons |= Net::InputFlags::SPRINT;
        if (actions.get("Crouch").held) state.buttons |= Net::InputFlags::CROUCH;
        if (actions.get("Walk").held) state.buttons |= Net::InputFlags::WALK;
        if (edges.drop) state.buttons |= Net::InputFlags::DROP;
        if (can_fire && actions.get("Fire").held) state.buttons |= Net::InputFlags::ATTACK;
        if (can_reload && edges.reload) state.buttons |= Net::InputFlags::RELOAD;

        // Keep the existing precedence when several weapon keys arrive in the
        // same rendered frame: the highest checked slot wins.
        if (edges.weapon_select != 0) state.buttons = Net::withWeaponSelect(state.buttons, edges.weapon_select);

        if (state.buttons & Net::InputFlags::MOVE_FORWARD) state.move_forward += 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_BACK) state.move_forward -= 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_RIGHT) state.move_right += 1.0f;
        if (state.buttons & Net::InputFlags::MOVE_LEFT) state.move_right -= 1.0f;
        return state;
    }

    // Compatibility helper for direct one-shot samplers and focused tests.
    // Shipping Game.exe uses FixedNetworkInputCadence above.
    inline Net::InputState sampleNetworkInput(
        const InputActionSystem& actions,
        float camera_yaw,
        float camera_pitch,
        bool local_alive,
        bool ui_modal,
        bool can_fire,
        bool can_reload)
    {
        NetworkInputEdges edges;
        edges.jump = actions.wasPressed("Jump");
        edges.drop = actions.wasPressed("Drop");
        edges.reload = actions.wasPressed("Reload");
        for (uint8_t slot = 1; slot <= 5; ++slot)
        {
            if (actions.wasPressed(FixedNetworkInputCadence::weaponAction(slot)))
                edges.weapon_select = slot;
        }
        return sampleNetworkInput(actions, camera_yaw, camera_pitch, local_alive,
            ui_modal, can_fire, can_reload, edges);
    }
}
