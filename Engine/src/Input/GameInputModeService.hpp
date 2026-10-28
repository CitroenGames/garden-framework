#pragma once

#include "InputManager.hpp"

#include <cstdint>
#include <functional>
#include <utility>

// Game modules request one of these host-owned pointer/input modes through
// EngineServices.  The host retains window and platform ownership; modules do
// not know whether relative mouse mode is implemented by SDL or another
// backend.
enum class GameInputMode : uint8_t
{
    GameplayPointerCapture,
    UiPointer,
};

class IGameInputModeService
{
public:
    virtual ~IGameInputModeService() = default;

    // Repeated requests for the current mode are successful no-ops.
    virtual bool requestGameInputMode(GameInputMode mode) = 0;
};

// Client-host policy shared by the SDL owner and focused InputTests.  It owns
// only generic input enablement and reset semantics; the callback performs
// the platform-specific pointer capture change.
class GameInputModeState
{
public:
    using ApplyPointerCapture = std::function<void(bool gameplay_pointer_capture)>;

    GameInputModeState(InputManager& input, ApplyPointerCapture apply_pointer_capture)
        : m_input(input)
        , m_apply_pointer_capture(std::move(apply_pointer_capture))
    {
    }

    bool request(GameInputMode mode)
    {
        if (m_requested_mode == mode)
            return true;

        m_requested_mode = mode;
        synchronize();
        return true;
    }

    void setHostUiActive(bool active)
    {
        if (m_host_ui_active == active)
            return;

        m_host_ui_active = active;
        synchronize();
    }

    void setWindowFocused(bool focused)
    {
        if (m_window_focused == focused)
            return;

        m_window_focused = focused;
        synchronize();
    }

    // Applies the initial gameplay-capture policy after the host window is
    // available.  It is also safe to call while the current policy is active.
    void synchronize()
    {
        const bool gameplay_enabled = gameplayInputEnabled();
        const bool keyboard_enabled = keyboardInputEnabled();
        if (m_policy_applied &&
            m_applied_gameplay_enabled == gameplay_enabled &&
            m_applied_keyboard_enabled == keyboard_enabled)
            return;

        // A mode change creates an input boundary.  Clear sampled physical
        // state in both directions so a key/button held during UI entry
        // cannot become gameplay intent when capture is restored.
        if (m_policy_applied)
            m_input.reset_state();

        const bool pointer_capture_changed =
            !m_policy_applied || m_applied_gameplay_enabled != gameplay_enabled;
        m_applied_gameplay_enabled = gameplay_enabled;
        m_applied_keyboard_enabled = keyboard_enabled;
        m_policy_applied = true;
        if (pointer_capture_changed && m_apply_pointer_capture)
            m_apply_pointer_capture(gameplay_enabled);
    }

    [[nodiscard]] GameInputMode requestedMode() const { return m_requested_mode; }
    [[nodiscard]] bool gameplayInputEnabled() const
    {
        return m_requested_mode == GameInputMode::GameplayPointerCapture &&
               !m_host_ui_active && m_window_focused;
    }

    // UI-pointer mode releases gameplay mouse/gamepad capture but retains
    // keyboard delivery for game modules that expose keyboard-first modal
    // commands.  The host UI and an unfocused window still retain keyboard
    // ownership, and callers apply their own UI capture policy as needed.
    [[nodiscard]] bool keyboardInputEnabled() const
    {
        return !m_host_ui_active && m_window_focused;
    }

private:
    InputManager& m_input;
    ApplyPointerCapture m_apply_pointer_capture;
    GameInputMode m_requested_mode = GameInputMode::GameplayPointerCapture;
    bool m_host_ui_active = false;
    bool m_window_focused = true;
    bool m_policy_applied = false;
    bool m_applied_gameplay_enabled = false;
    bool m_applied_keyboard_enabled = false;
};
