#pragma once

#include "gameObject.hpp"
#include "camera.hpp"
#include "SDL.h"

#include <cstring>
#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/quaternion.h"

class freecamEntity : public component
{
public:
    camera& freecam_camera;
    bool w, a, s, d, space, shift;
    
    float movement_speed;
    float fast_movement_speed;
    float mouse_sensitivity;
    
    freecamEntity(camera& fc, gameObject& obj) 
        : component(obj), freecam_camera(fc)
    {
        w = a = s = d = space = shift = false;
        movement_speed = 5.0f;         // Normal movement speed
        fast_movement_speed = 15.0f;   // Fast movement speed when holding shift
        mouse_sensitivity = 1.0f;
    }
    
    void update_camera(float yrel, float xrel)
    {
        freecam_camera.rotation.X += yrel / 1000.0f * mouse_sensitivity;
        freecam_camera.rotation.Y += -xrel / 1000.0f * mouse_sensitivity;
        
        // Clamp pitch to prevent camera flipping
        freecam_camera.rotation.X = clamp<float>(freecam_camera.rotation.X, -1.5f, 1.5f);
    }

    void handle_input_up(SDL_Keysym* keysym)
    {
        switch (keysym->sym)
        {
        case SDLK_w:
            w = false;
            break;
        case SDLK_a:
            a = false;
            break;
        case SDLK_s:
            s = false;
            break;
        case SDLK_d:
            d = false;
            break;
        case SDLK_SPACE:
            space = false;
            break;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
            shift = false;
            break;
        }
    }
    
    void handle_input_down(SDL_Keysym* keysym)
    {
        switch (keysym->sym)
        {
        case SDLK_w:
            w = true;
            break;
        case SDLK_a:
            a = true;
            break;
        case SDLK_s:
            s = true;
            break;
        case SDLK_d:
            d = true;
            break;
        case SDLK_SPACE:
            space = true;
            break;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
            shift = true;
            break;
        }
    }
    
    void update_freecam(float delta)
    {
        // Calculate movement direction in local camera space
        vector3f local_movement = vector3f(0, 0, 0);
        
        if (w) local_movement.Z += 1.0f;  // Forward
        if (s) local_movement.Z -= 1.0f;  // Backward
        if (a) local_movement.X += 1.0f;  // Left
        if (d) local_movement.X -= 1.0f;  // Right
        if (space) local_movement.Y += 1.0f;    // Up
        if (shift) local_movement.Y -= 1.0f;    // Down
        
        // Normalize movement vector to prevent faster diagonal movement
        if (local_movement.getLength() > 0)
        {
            local_movement = local_movement.normalize();
        }
        
        // Determine current movement speed
        float current_speed = shift ? fast_movement_speed : movement_speed;
        
        // Transform movement from local camera space to world space
        vector3f world_movement = freecam_camera.camera_rot_quaternion() * local_movement;
        
        // Apply movement
        freecam_camera.position += world_movement * current_speed * delta;
        
        // Update the game object position to match camera
        obj.position = freecam_camera.position;
    }
};