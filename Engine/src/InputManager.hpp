#pragma once

#include "EngineExport.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Simple input action states
enum class InputActionState
{
    Pressed,    // Just pressed this frame
    Released,   // Just released this frame
    Held        // Held down
};

// Action delegate type
using ActionDelegate = std::function<void(InputActionState state)>;

// Input mapping structures
struct ActionMapping
{
    std::string action_name;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
};

class ENGINE_API InputManager
{
private:
    static constexpr std::size_t KeyCount = SDL_SCANCODE_COUNT;
    static constexpr std::size_t MouseButtonCount = 256;

    // Direct key state tracking (no keyboard repeat issues).
    std::array<bool, KeyCount> current_key_states{};
    std::array<bool, KeyCount> previous_key_states{};

    // Mouse button state tracking (SDL3 button ids: 1=L, 2=M, 3=R, 4=X1, 5=X2).
    std::array<bool, MouseButtonCount> current_mouse_button_states{};
    std::array<bool, MouseButtonCount> previous_mouse_button_states{};

    // Mouse data
    float mouse_delta_x = 0.0f;
    float mouse_delta_y = 0.0f;
    
    // Input mappings
    std::vector<ActionMapping> action_mappings;
    
    // Delegates
    std::unordered_map<std::string, std::vector<ActionDelegate>> action_delegates;

    static bool is_valid_key(SDL_Scancode key);
    void dispatch_action(SDL_Scancode key, InputActionState state) const;
    void set_key_state(SDL_Scancode key, bool is_down);
    void set_mouse_button_state(uint8_t button, bool is_down);

public:
    // Mouse sensitivity settings - now public and properly used
    float Sensitivity_X = 3.0f;
    float Sensitivity_Y = 3.0f;
    
    InputManager();
    ~InputManager() = default;
    
    // Core update functions
    void update();  // Call at start of frame
    void process_event(const SDL_Event& event);
    
    // Action binding
    void bind_action(const std::string& action_name, ActionDelegate delegate);
    void add_action_mapping(const std::string& action_name, SDL_Scancode key);
    
    // Direct key queries (the good stuff that actually works)
    bool is_key_pressed(SDL_Scancode key) const;
    bool is_key_released(SDL_Scancode key) const;
    bool is_key_held(SDL_Scancode key) const;

    // Direct mouse button queries (button: 1=L, 2=M, 3=R, 4=X1, 5=X2 — SDL3 BUTTON_* ids)
    bool is_mouse_button_pressed(uint8_t button) const;
    bool is_mouse_button_released(uint8_t button) const;
    bool is_mouse_button_held(uint8_t button) const;
    
    // Mouse input - raw deltas
    float get_mouse_delta_x() const { return mouse_delta_x; }
    float get_mouse_delta_y() const { return mouse_delta_y; }
    
    // Mouse sensitivity scaling - these are now the primary methods to use
    float get_scaled_mouse_delta_x() const { return mouse_delta_x * Sensitivity_X; }
    float get_scaled_mouse_delta_y() const { return mouse_delta_y * Sensitivity_Y; }
    
    // Mouse sensitivity control methods
    void set_mouse_sensitivity(float sensitivity)
    {
        Sensitivity_X = sensitivity;
        Sensitivity_Y = sensitivity;
    }
    
    void set_mouse_sensitivity_x(float sensitivity)
    {
        Sensitivity_X = sensitivity;
    }
    
    void set_mouse_sensitivity_y(float sensitivity)
    {
        Sensitivity_Y = sensitivity;
    }
    
    void set_mouse_sensitivity_xy(float sensitivity_x, float sensitivity_y)
    {
        Sensitivity_X = sensitivity_x;
        Sensitivity_Y = sensitivity_y;
    }
    
    float get_mouse_sensitivity_x() const { return Sensitivity_X; }
    float get_mouse_sensitivity_y() const { return Sensitivity_Y; }
    
    // Utility
    void reset_state();
    void clear_all_mappings();
    void setup_default_mappings();
};
