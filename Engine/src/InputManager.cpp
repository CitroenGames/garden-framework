#include "InputManager.hpp"

#include <utility>

InputManager::InputManager()
{
    setup_default_mappings();
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
    current_key_states[index] = is_down;

    if (is_down && !was_down)
        dispatch_action(key, InputActionState::Pressed);
    else if (!is_down && was_down)
        dispatch_action(key, InputActionState::Released);
}

void InputManager::set_mouse_button_state(uint8_t button, bool is_down)
{
    current_mouse_button_states[static_cast<std::size_t>(button)] = is_down;
}

void InputManager::update()
{
    // Update previous key states
    previous_key_states = current_key_states;
    previous_mouse_button_states = current_mouse_button_states;

    // Reset mouse deltas
    mouse_delta_x = 0.0f;
    mouse_delta_y = 0.0f;
}

void InputManager::process_event(const SDL_Event& event)
{
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
    }
}

void InputManager::reset_state()
{
    current_key_states.fill(false);
    previous_key_states.fill(false);
    current_mouse_button_states.fill(false);
    previous_mouse_button_states.fill(false);
    mouse_delta_x = 0.0f;
    mouse_delta_y = 0.0f;
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
