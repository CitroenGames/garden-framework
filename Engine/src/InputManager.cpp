#include "InputManager.hpp"
#include "MovementTrace.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr float StickDeadZone = 0.10f;
    constexpr float TriggerDeadZone = 0.05f;

    bool isTriggerAxis(int axis)
    {
        return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }
}

InputManager::InputManager()
{
    setup_default_mappings();
}

InputManager::~InputManager()
{
    for (GamepadDevice& device : gamepads)
    {
        if (device.handle)
            SDL_CloseGamepad(device.handle);
        device.handle = nullptr;
    }
}

void InputManager::initialize_gamepads()
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    int count = 0;
    SDL_JoystickID* instance_ids = SDL_GetGamepads(&count);
    if (!instance_ids)
        return;

    for (int index = 0; index < count; ++index)
        add_gamepad(instance_ids[index]);

    SDL_free(instance_ids);
}

bool InputManager::is_valid_key(SDL_Scancode key)
{
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT;
}

void InputManager::dispatch_action(SDL_Scancode key, InputActionState state) const
{
    for (const auto& mapping : action_mappings)
    {
        if (mapping.key != key)
            continue;

        auto it = action_delegates.find(mapping.action_name);
        if (it == action_delegates.end())
            continue;

        for (const auto& delegate : it->second)
        {
            if (delegate)
                delegate(state);
        }
    }
}

void InputManager::set_key_state(SDL_Scancode key, bool is_down)
{
    if (!is_valid_key(key))
        return;

    const std::size_t index = static_cast<std::size_t>(key);
    const bool was_down = current_key_states[index];
    if (was_down == is_down)
        return;

    current_key_states[index] = is_down;
    button_transitions.push_back({ RawInputDevice::Keyboard, static_cast<int>(key), 0, is_down,
        was_down ? 1.0f : 0.0f, is_down ? 1.0f : 0.0f });

    if (is_down)
        dispatch_action(key, InputActionState::Pressed);
    else
        dispatch_action(key, InputActionState::Released);
}

void InputManager::set_mouse_button_state(uint8_t button, bool is_down)
{
    const std::size_t index = static_cast<std::size_t>(button);
    const bool was_down = current_mouse_button_states[index];
    if (was_down == is_down)
        return;

    current_mouse_button_states[index] = is_down;
    button_transitions.push_back({ RawInputDevice::Mouse, static_cast<int>(button), 0, is_down,
        was_down ? 1.0f : 0.0f, is_down ? 1.0f : 0.0f });
}

void InputManager::set_gamepad_button_state(std::size_t slot, int button, bool is_down)
{
    if (slot >= gamepads.size() || button < 0 || button >= 32 ||
        !gamepads[slot].state.connected)
        return;

    GamepadState& state = gamepads[slot].state;
    const uint32_t mask = uint32_t{ 1 } << static_cast<uint32_t>(button);
    const bool was_down = (state.buttons_held & mask) != 0;
    if (was_down == is_down)
        return;

    if (is_down)
    {
        state.buttons_held |= mask;
        state.buttons_pressed_mask |= mask;
    }
    else
    {
        state.buttons_held &= ~mask;
        state.buttons_released_mask |= mask;
    }

    button_transitions.push_back({ RawInputDevice::GamepadButton, button, static_cast<int>(slot), is_down,
        was_down ? 1.0f : 0.0f, is_down ? 1.0f : 0.0f });
}

float InputManager::normalize_gamepad_axis(int axis, Sint16 value)
{
    if (isTriggerAxis(axis))
        return std::clamp(static_cast<float>(value) / 32767.0f, 0.0f, 1.0f);
    if (value < 0)
        return std::clamp(static_cast<float>(value) / 32768.0f, -1.0f, 1.0f);
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

float InputManager::apply_trigger_deadzone(float value)
{
    if (value <= TriggerDeadZone)
        return 0.0f;
    return std::clamp((value - TriggerDeadZone) / (1.0f - TriggerDeadZone), 0.0f, 1.0f);
}

glm::vec2 InputManager::apply_stick_deadzone(glm::vec2 value)
{
    const float magnitude = glm::length(value);
    if (magnitude <= StickDeadZone)
        return glm::vec2(0.0f);

    const float clamped_magnitude = std::min(magnitude, 1.0f);
    const float rescaled_magnitude = (clamped_magnitude - StickDeadZone) / (1.0f - StickDeadZone);
    return value * (rescaled_magnitude / magnitude);
}

void InputManager::refresh_gamepad_axes(std::size_t slot)
{
    GamepadDevice& device = gamepads[slot];
    const glm::vec2 left = apply_stick_deadzone(glm::vec2(
        device.raw_axes[SDL_GAMEPAD_AXIS_LEFTX], device.raw_axes[SDL_GAMEPAD_AXIS_LEFTY]));
    const glm::vec2 right = apply_stick_deadzone(glm::vec2(
        device.raw_axes[SDL_GAMEPAD_AXIS_RIGHTX], device.raw_axes[SDL_GAMEPAD_AXIS_RIGHTY]));

    device.state.left_stick = left;
    device.state.right_stick = right;
    device.state.left_trigger = apply_trigger_deadzone(device.raw_axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
    device.state.right_trigger = apply_trigger_deadzone(device.raw_axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    device.state.axes[SDL_GAMEPAD_AXIS_LEFTX] = left.x;
    device.state.axes[SDL_GAMEPAD_AXIS_LEFTY] = left.y;
    device.state.axes[SDL_GAMEPAD_AXIS_RIGHTX] = right.x;
    device.state.axes[SDL_GAMEPAD_AXIS_RIGHTY] = right.y;
    device.state.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = device.state.left_trigger;
    device.state.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = device.state.right_trigger;
}

void InputManager::set_gamepad_axis_state(std::size_t slot, int axis, Sint16 value)
{
    if (!gamepad_input_enabled || slot >= gamepads.size() || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT ||
        !gamepads[slot].state.connected)
        return;

    GamepadDevice& device = gamepads[slot];
    const auto previous_axes = device.state.axes;
    device.raw_axes[static_cast<std::size_t>(axis)] = normalize_gamepad_axis(axis, value);
    refresh_gamepad_axes(slot);

    for (int changed_axis = 0; changed_axis < SDL_GAMEPAD_AXIS_COUNT; ++changed_axis)
    {
        const float previous = previous_axes[static_cast<std::size_t>(changed_axis)];
        const float current = device.state.axes[static_cast<std::size_t>(changed_axis)];
        if (std::abs(previous - current) <= 0.00001f)
            continue;

        button_transitions.push_back({ RawInputDevice::GamepadAxis, changed_axis, static_cast<int>(slot),
            std::abs(current) > 0.0001f, previous, current });
    }
}

void InputManager::add_gamepad(SDL_JoystickID instance_id)
{
    if (instance_id == 0 || !SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    for (const GamepadDevice& device : gamepads)
    {
        if (device.handle && device.instance_id == instance_id)
            return;
    }

    auto slot = std::find_if(gamepads.begin(), gamepads.end(), [](const GamepadDevice& device) {
        return device.handle == nullptr;
    });
    if (slot == gamepads.end())
        return;

    SDL_Gamepad* handle = SDL_OpenGamepad(instance_id);
    if (!handle)
        return;

    slot->handle = handle;
    slot->instance_id = instance_id;
    slot->state = {};
    slot->state.connected = true;
    slot->raw_axes.fill(0.0f);
    refresh_gamepad_state(static_cast<std::size_t>(std::distance(gamepads.begin(), slot)));
}

void InputManager::clear_gamepad_state(std::size_t slot, bool disconnected)
{
    if (slot >= gamepads.size())
        return;

    GamepadDevice& device = gamepads[slot];
    GamepadState& state = device.state;
    const uint32_t pressed_mask = state.buttons_pressed_mask;
    uint32_t released_mask = state.buttons_released_mask;
    for (int button = 0; button < 32; ++button)
    {
        const uint32_t mask = uint32_t{ 1 } << static_cast<uint32_t>(button);
        if ((state.buttons_held & mask) == 0)
            continue;
        released_mask |= mask;
        button_transitions.push_back({ RawInputDevice::GamepadButton, button, static_cast<int>(slot), false,
            1.0f, 0.0f });
    }

    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
    {
        const float previous = state.axes[static_cast<std::size_t>(axis)];
        if (std::abs(previous) <= 0.00001f)
            continue;
        button_transitions.push_back({ RawInputDevice::GamepadAxis, axis, static_cast<int>(slot), false,
            previous, 0.0f });
    }

    const bool was_connected = state.connected;
    state = {};
    state.connected = was_connected && !disconnected;
    state.buttons_pressed_mask = pressed_mask;
    state.buttons_released_mask = released_mask;
    device.raw_axes.fill(0.0f);
}

void InputManager::remove_gamepad(SDL_JoystickID instance_id)
{
    for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
    {
        GamepadDevice& device = gamepads[slot];
        if (!device.handle || device.instance_id != instance_id)
            continue;

        clear_gamepad_state(slot, true);
        SDL_CloseGamepad(device.handle);
        device.handle = nullptr;
        device.instance_id = 0;
        return;
    }
}

void InputManager::refresh_gamepad_state(std::size_t slot)
{
    if (slot >= gamepads.size() || !gamepads[slot].handle || !gamepad_input_enabled)
        return;

    GamepadDevice& device = gamepads[slot];
    device.state.buttons_held = 0;
    device.state.buttons_pressed_mask = 0;
    device.state.buttons_released_mask = 0;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && button < 32; ++button)
    {
        if (SDL_GetGamepadButton(device.handle, static_cast<SDL_GamepadButton>(button)))
            device.state.buttons_held |= uint32_t{ 1 } << static_cast<uint32_t>(button);
    }

    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
    {
        device.raw_axes[static_cast<std::size_t>(axis)] = normalize_gamepad_axis(
            axis, SDL_GetGamepadAxis(device.handle, static_cast<SDL_GamepadAxis>(axis)));
    }
    refresh_gamepad_axes(slot);
}

void InputManager::set_gamepad_input_enabled(bool enabled)
{
    if (gamepad_input_enabled == enabled)
        return;

    gamepad_input_enabled = enabled;
    if (!enabled)
    {
        for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
            clear_gamepad_state(slot, false);
        return;
    }

    for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
        refresh_gamepad_state(slot);
}

void InputManager::update()
{
    // Update previous key states
    previous_key_states = current_key_states;
    previous_mouse_button_states = current_mouse_button_states;
    button_transitions.clear();
    for (GamepadDevice& device : gamepads)
    {
        device.state.buttons_pressed_mask = 0;
        device.state.buttons_released_mask = 0;
    }

    // Reset mouse deltas
    mouse_delta_x = 0.0f;
    mouse_delta_y = 0.0f;
}

void InputManager::process_event(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        MovementTrace::emit("physical_key", "role=client frame_id=unavailable scancode={} down={} repeat={} sdl_event_ns={} event_clock=sdl sdl_to_steady_mapping=unavailable",
            static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN,
            event.key.repeat, event.key.timestamp);
    const bool trace_key = MovementTrace::enabled() && MovementTrace::movementScope() &&
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP);
    const bool key_valid = trace_key && is_valid_key(event.key.scancode);
    const bool key_before = key_valid && is_key_held(event.key.scancode);
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat)
            set_key_state(event.key.scancode, true);
        break;

    case SDL_EVENT_KEY_UP:
        set_key_state(event.key.scancode, false);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        mouse_delta_x += event.motion.xrel;
        mouse_delta_y += event.motion.yrel;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        set_mouse_button_state(event.button.button, true);
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        set_mouse_button_state(event.button.button, false);
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
        add_gamepad(event.gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(event.gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (gamepad_input_enabled)
        {
            for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
            {
                if (gamepads[slot].handle && gamepads[slot].instance_id == event.gbutton.which)
                {
                    set_gamepad_button_state(slot, event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                    break;
                }
            }
        }
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (gamepad_input_enabled)
        {
            for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
            {
                if (gamepads[slot].handle && gamepads[slot].instance_id == event.gaxis.which)
                {
                    set_gamepad_axis_state(slot, event.gaxis.axis, event.gaxis.value);
                    break;
                }
            }
        }
        break;

    case SDL_EVENT_GAMEPAD_REMAPPED:
        for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
        {
            if (gamepads[slot].handle && gamepads[slot].instance_id == event.gdevice.which)
            {
                refresh_gamepad_state(slot);
                break;
            }
        }
        break;
    }
    if (trace_key)
        MovementTrace::emit("key_state", "role=client scancode={} down={} repeat={} sdl_event_ns={} event_clock=sdl valid={} held_before={} held_after={} transition_added={}",
            static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN, event.key.repeat,
            event.key.timestamp, key_valid, key_before, key_valid && is_key_held(event.key.scancode),
            key_valid && key_before != is_key_held(event.key.scancode));
}

void InputManager::reset_state()
{
    for (std::size_t index = 0; index < current_key_states.size(); ++index)
    {
        if (current_key_states[index])
            button_transitions.push_back({ RawInputDevice::Keyboard, static_cast<int>(index), 0, false, 1.0f, 0.0f });
    }
    for (std::size_t index = 0; index < current_mouse_button_states.size(); ++index)
    {
        if (current_mouse_button_states[index])
            button_transitions.push_back({ RawInputDevice::Mouse, static_cast<int>(index), 0, false, 1.0f, 0.0f });
    }

    current_key_states.fill(false);
    previous_key_states.fill(false);
    current_mouse_button_states.fill(false);
    previous_mouse_button_states.fill(false);
    mouse_delta_x = 0.0f;
    mouse_delta_y = 0.0f;
    for (std::size_t slot = 0; slot < gamepads.size(); ++slot)
        clear_gamepad_state(slot, false);
}

const GamepadState& InputManager::gamepad(int slot) const
{
    static const GamepadState disconnected{};
    if (slot < 0 || static_cast<std::size_t>(slot) >= gamepads.size())
        return disconnected;
    return gamepads[static_cast<std::size_t>(slot)].state;
}

void InputManager::bind_action(const std::string& action_name, ActionDelegate delegate)
{
    action_delegates[action_name].push_back(delegate);
}

void InputManager::add_action_mapping(const std::string& action_name, SDL_Scancode key)
{
    if (!is_valid_key(key))
        return;

    ActionMapping mapping;
    mapping.action_name = action_name;
    mapping.key = key;
    action_mappings.push_back(std::move(mapping));
}

bool InputManager::is_key_pressed(SDL_Scancode key) const
{
    if (!is_valid_key(key))
        return false;

    const std::size_t index = static_cast<std::size_t>(key);
    return current_key_states[index] && !previous_key_states[index];
}

bool InputManager::is_key_released(SDL_Scancode key) const
{
    if (!is_valid_key(key))
        return false;

    const std::size_t index = static_cast<std::size_t>(key);
    return !current_key_states[index] && previous_key_states[index];
}

bool InputManager::is_key_held(SDL_Scancode key) const
{
    if (!is_valid_key(key))
        return false;

    return current_key_states[static_cast<std::size_t>(key)];
}

bool InputManager::is_mouse_button_pressed(uint8_t button) const
{
    const std::size_t index = static_cast<std::size_t>(button);
    return current_mouse_button_states[index] && !previous_mouse_button_states[index];
}

bool InputManager::is_mouse_button_released(uint8_t button) const
{
    const std::size_t index = static_cast<std::size_t>(button);
    return !current_mouse_button_states[index] && previous_mouse_button_states[index];
}

bool InputManager::is_mouse_button_held(uint8_t button) const
{
    return current_mouse_button_states[static_cast<std::size_t>(button)];
}

void InputManager::clear_all_mappings()
{
    action_mappings.clear();
    action_delegates.clear();
}

void InputManager::setup_default_mappings()
{
    action_mappings.clear();

    // Basic actions only - movement handled directly
    add_action_mapping("ToggleFreecam", SDL_SCANCODE_F);
    add_action_mapping("Quit", SDL_SCANCODE_ESCAPE);
}
