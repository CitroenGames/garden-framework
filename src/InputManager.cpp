#include "InputManager.hpp"

InputManager::InputManager()
{
    setup_default_mappings();
}

void InputManager::update()
{
    // Update previous key states
    previous_key_states = current_key_states;
    
    // Reset mouse deltas
    mouse_delta_x = 0.0f;
    mouse_delta_y = 0.0f;
}

void InputManager::process_event(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_KEYDOWN:
        if (!event.key.repeat) // Ignore key repeat events
        {
            current_key_states[event.key.keysym.scancode] = true;
            
            // Fire action pressed delegates
            for (const auto& mapping : action_mappings)
            {
                if (mapping.key == event.key.keysym.scancode)
                {
                    auto it = action_delegates.find(mapping.action_name);
                    if (it != action_delegates.end())
                    {
                        for (const auto& delegate : it->second)
                        {
                            delegate(InputActionState::Pressed);
                        }
                    }
                }
            }
        }
        break;
        
    case SDL_KEYUP:
        current_key_states[event.key.keysym.scancode] = false;
        
        // Fire action released delegates
        for (const auto& mapping : action_mappings)
        {
            if (mapping.key == event.key.keysym.scancode)
            {
                auto it = action_delegates.find(mapping.action_name);
                if (it != action_delegates.end())
                {
                    for (const auto& delegate : it->second)
                    {
                        delegate(InputActionState::Released);
                    }
                }
            }
        }
        break;
        
    case SDL_MOUSEMOTION:
        mouse_delta_x = static_cast<float>(event.motion.xrel);
        mouse_delta_y = static_cast<float>(event.motion.yrel);
        break;
    }
}

void InputManager::bind_action(const std::string& action_name, ActionDelegate delegate)
{
    action_delegates[action_name].push_back(delegate);
}

void InputManager::add_action_mapping(const std::string& action_name, SDL_Scancode key)
{
    ActionMapping mapping;
    mapping.action_name = action_name;
    mapping.key = key;
    action_mappings.push_back(mapping);
}

bool InputManager::is_key_pressed(SDL_Scancode key) const
{
    auto current_it = current_key_states.find(key);
    auto previous_it = previous_key_states.find(key);
    
    bool current_pressed = (current_it != current_key_states.end()) ? current_it->second : false;
    bool previous_pressed = (previous_it != previous_key_states.end()) ? previous_it->second : false;
    
    return current_pressed && !previous_pressed;
}

bool InputManager::is_key_released(SDL_Scancode key) const
{
    auto current_it = current_key_states.find(key);
    auto previous_it = previous_key_states.find(key);
    
    bool current_pressed = (current_it != current_key_states.end()) ? current_it->second : false;
    bool previous_pressed = (previous_it != previous_key_states.end()) ? previous_it->second : false;
    
    return !current_pressed && previous_pressed;
}

bool InputManager::is_key_held(SDL_Scancode key) const
{
    auto it = current_key_states.find(key);
    return (it != current_key_states.end()) ? it->second : false;
}

void InputManager::clear_all_mappings()
{
    action_mappings.clear();
    action_delegates.clear();
}

void InputManager::setup_default_mappings()
{
    // Basic actions only - movement handled directly
    add_action_mapping("ToggleFreecam", SDL_SCANCODE_F);
    add_action_mapping("Quit", SDL_SCANCODE_ESCAPE);
}