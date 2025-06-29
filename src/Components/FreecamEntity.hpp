#pragma once

#include "Components/gameObject.hpp"
#include "Components/camera.hpp"
#include "InputManager.hpp"
#include "SDL.h"

#include <cstring>
#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/quaternion.h"

class freecamEntity : public component
{
public:
    camera& freecam_camera;
    std::shared_ptr<InputManager> input_manager;
    bool input_enabled = true;
    
    float movement_speed;
    float fast_movement_speed;
    float mouse_sensitivity; // Local multiplier for InputManager sensitivity
    
    freecamEntity(camera& fc, gameObject& obj, std::shared_ptr<InputManager> input_mgr) 
        : component(obj), freecam_camera(fc), input_manager(input_mgr)
    {
        movement_speed = 5.0f;         // Normal movement speed
        fast_movement_speed = 15.0f;   // Fast movement speed when holding shift
        mouse_sensitivity = 1.0f;      // Local multiplier for InputManager sensitivity
    }
    
    void set_input_enabled(bool enabled) { input_enabled = enabled; }
    bool is_input_enabled() const { return input_enabled; }
    
    void update_camera(float yrel, float xrel)
    {
        if (!input_enabled) return;
        
        // Apply InputManager sensitivity scaling to the provided deltas
        float effective_sensitivity_x = input_manager ? input_manager->Sensitivity_X * mouse_sensitivity : mouse_sensitivity;
        float effective_sensitivity_y = input_manager ? input_manager->Sensitivity_Y * mouse_sensitivity : mouse_sensitivity;
        
        freecam_camera.rotation.X += yrel / 1000.0f * effective_sensitivity_y;
        freecam_camera.rotation.Y += -xrel / 1000.0f * effective_sensitivity_x;
        
        // Clamp pitch to prevent camera flipping
        freecam_camera.rotation.X = clamp<float>(freecam_camera.rotation.X, -1.5f, 1.5f);
    }
    
    // Version that uses InputManager's mouse deltas directly
    void update_camera_from_input_manager()
    {
        if (!input_enabled || !input_manager) return;
        
        // Get raw mouse deltas from InputManager and apply combined sensitivity
        float yrel = input_manager->get_mouse_delta_y();
        float xrel = input_manager->get_mouse_delta_x();
        
        if (yrel == 0.0f && xrel == 0.0f) return; // No mouse movement this frame
        
        float effective_sensitivity_x = input_manager->Sensitivity_X * mouse_sensitivity;
        float effective_sensitivity_y = input_manager->Sensitivity_Y * mouse_sensitivity;
        
        freecam_camera.rotation.X += yrel / 1000.0f * effective_sensitivity_y;
        freecam_camera.rotation.Y += -xrel / 1000.0f * effective_sensitivity_x;
        
        // Clamp pitch to prevent camera flipping
        freecam_camera.rotation.X = clamp<float>(freecam_camera.rotation.X, -1.5f, 1.5f);
    }
    
    void update_freecam(float delta)
    {
        if (!input_enabled || !input_manager) return;
        
        // Update camera with mouse input from InputManager
        update_camera_from_input_manager();
        
        // Calculate movement direction in local camera space using direct key queries
        vector3f local_movement = vector3f(0, 0, 0);
        
        if (input_manager->is_key_held(SDL_SCANCODE_W)) local_movement.Z += 1.0f;  // Forward
        if (input_manager->is_key_held(SDL_SCANCODE_S)) local_movement.Z -= 1.0f;  // Backward
        if (input_manager->is_key_held(SDL_SCANCODE_A)) local_movement.X += 1.0f;  // Left
        if (input_manager->is_key_held(SDL_SCANCODE_D)) local_movement.X -= 1.0f;  // Right
        if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) local_movement.Y += 1.0f;    // Up
        if (input_manager->is_key_held(SDL_SCANCODE_LSHIFT)) local_movement.Y -= 1.0f;    // Down
        
        // Normalize movement vector to prevent faster diagonal movement
        if (local_movement.getLength() > 0)
        {
            local_movement = local_movement.normalize();
        }
        
        // Determine current movement speed (using shift for fast movement)
        float current_speed = input_manager->is_key_held(SDL_SCANCODE_LSHIFT) ? fast_movement_speed : movement_speed;
        
        // Transform movement from local camera space to world space
        vector3f world_movement = freecam_camera.camera_rot_quaternion() * local_movement;
        
        // Apply movement
        freecam_camera.position += world_movement * current_speed * delta;
        
        // Update the game object position to match camera
        obj.position = freecam_camera.position;
    }
    
    // Get effective mouse sensitivity (InputManager sensitivity * local multiplier)
    float get_effective_mouse_sensitivity_x() const
    {
        if (!input_manager) return mouse_sensitivity;
        return input_manager->Sensitivity_X * mouse_sensitivity;
    }
    
    float get_effective_mouse_sensitivity_y() const
    {
        if (!input_manager) return mouse_sensitivity;
        return input_manager->Sensitivity_Y * mouse_sensitivity;
    }
    
    // Legacy methods for backward compatibility
    void handle_input_up(SDL_Keysym* keysym) { /* Deprecated */ }
    void handle_input_down(SDL_Keysym* keysym) { /* Deprecated */ }
};