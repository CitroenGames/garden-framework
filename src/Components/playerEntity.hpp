#pragma once

#include "gameObject.hpp"
#include "camera.hpp"
#include "rigidbody.hpp"
#include "InputManager.hpp"
#include "SDL.h"

#include <cstring>
#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/quaternion.h"
#include "irrlicht/irrMath.h"

using namespace irr;
using namespace core;

class playerEntity : public component
{
public:
    camera& player_camera;
    rigidbody& player_rb;
    std::shared_ptr<InputManager> input_manager;
    bool input_enabled = true;

    float speed;
    float jump_force;
    float mouse_sensitivity;

    // updated by the world class whilst colliding
    vector3f ground_normal;
    bool grounded;
    void update_ground_normal(vector3f n) { ground_normal = n; }
    void update_grounded(bool g) { grounded = g; }

    playerEntity(camera& pc, rigidbody& prb, gameObject& obj, std::shared_ptr<InputManager> input_mgr) 
        : component(obj), player_camera(pc), player_rb(prb), input_manager(input_mgr)
    {
        speed = 1.5f;
        jump_force = 3.0f;
        mouse_sensitivity = 1.0f;
    }

    void set_input_enabled(bool enabled) { input_enabled = enabled; }
    bool is_input_enabled() const { return input_enabled; }

    void update_camera(float yrel, float xrel)
    {
        if (!input_enabled) return;
        
        player_camera.rotation.X += yrel / 1000.0f * mouse_sensitivity;
        player_camera.rotation.Y += -xrel / 1000.0f * mouse_sensitivity;

        // Clamp pitch to prevent camera flipping - use explicit float values
        player_camera.rotation.X = clamp<float>(player_camera.rotation.X, -1.5f, 1.5f);
    }
    
    void update_player(float delta)
    {
        if (!input_enabled || !input_manager) return;
        
        // Get movement input using direct key queries
        float move_forward = 0.0f;
        float move_right = 0.0f;
        
        if (input_manager->is_key_held(SDL_SCANCODE_W)) move_forward += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) move_forward -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) move_right -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) move_right += 1.0f;
        
        vector3f wish_dir = vector3f(move_right, 0, move_forward);

        // (ive done this so many times its basically second nature rn)
        // rotate wish_dir by camera
        wish_dir = player_camera.camera_rot_quaternion() * wish_dir;

        if (grounded)
        {
            // project on the ground normal
            wish_dir.projectOnPlane(ground_normal);
        }
        else
        {
            wish_dir.Y = 0;
        }
        
        // finally normalize
        wish_dir = wish_dir.normalize();
        
        // check for jumping using direct key query
        bool wishJump = input_manager->is_key_held(SDL_SCANCODE_SPACE);
        bool jump = wishJump && grounded;   // conditions for jumping
        
        player_rb.velocity += wish_dir * speed * delta;
        if (grounded)
        {
            player_rb.velocity *= 0.6f; // friction
            if (jump)
                player_rb.velocity.Y = jump_force;
            else
                player_rb.velocity.Y = 0; // stick to surfaces
        }
        else
        {
            player_rb.velocity *= 0.7f; // air friction
            player_rb.velocity.Y -= 2.0f * delta;
        }
        
        player_camera.position = obj.position.getInterpolated(player_camera.position, delta);
    }
    
    // Legacy methods for backward compatibility
    void handle_input_up(SDL_Keysym* keysym) { /* Deprecated */ }
    void handle_input_down(SDL_Keysym* keysym) { /* Deprecated */ }
};