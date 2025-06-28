#pragma once

#include "Components/gameObject.hpp"
#include "Components/camera.hpp"
#include "Components/playerEntity.hpp"
#include "freecamEntity.hpp"
#include "InputManager.hpp"

enum class PossessedEntityType
{
    Player,
    Freecam
};

class PlayerController
{
private:
    playerEntity* player_entity;
    freecamEntity* freecam_entity;
    std::shared_ptr<InputManager> input_manager;
    PossessedEntityType currently_possessed;
    bool freecam_mode_enabled;

public:
    PlayerController(std::shared_ptr<InputManager> input_mgr) 
        : input_manager(input_mgr), player_entity(nullptr), freecam_entity(nullptr), 
          currently_possessed(PossessedEntityType::Player), 
          freecam_mode_enabled(false) 
    {
        setup_input_bindings();
    }

    void setup_input_bindings()
    {
        if (!input_manager) return;
        
        // Bind freecam toggle
        input_manager->bind_action("ToggleFreecam", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                toggleFreecamMode();
            }
        });
        
        // Bind quit action
        input_manager->bind_action("Quit", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                quit_game();
            }
        });
    }

    void setPossessedPlayer(playerEntity* player)
    {
        player_entity = player;
    }

    void setPossessedFreecam(freecamEntity* freecam)
    {
        freecam_entity = freecam;
    }

    void toggleFreecamMode()
    {
        if (!player_entity || !freecam_entity)
            return;

        freecam_mode_enabled = !freecam_mode_enabled;

        if (freecam_mode_enabled)
        {
            // Switch to freecam
            currently_possessed = PossessedEntityType::Freecam;
            
            // Disable player input and enable freecam input
            player_entity->set_input_enabled(false);
            freecam_entity->set_input_enabled(true);
            
            // Position freecam at player's current location
            freecam_entity->obj.position = player_entity->obj.position;
            freecam_entity->freecam_camera.position = player_entity->player_camera.position;
            freecam_entity->freecam_camera.rotation = player_entity->player_camera.rotation;
            
            printf("Switched to freecam mode\n");
        }
        else
        {
            // Switch back to player
            currently_possessed = PossessedEntityType::Player;
            
            // Enable player input and disable freecam input
            player_entity->set_input_enabled(true);
            freecam_entity->set_input_enabled(false);
            
            printf("Switched to player mode\n");
        }
    }

    // Handle mouse motion for the currently active entity
    // Note: Use this if you're passing mouse deltas manually from your main loop
    void handleMouseMotion(float yrel, float xrel)
    {
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            // If playerEntity has a similar update_camera method, call it here
            // Otherwise, you may need to implement mouse handling in playerEntity
            // player_entity->update_camera(yrel, xrel);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->update_camera(yrel, xrel);
        }
    }

    // Update currently possessed entity
    void update(float delta)
    {
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            // Assuming playerEntity has an update_player method
            player_entity->update_player(delta);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->update_freecam(delta);
        }
    }
    
    // Alternative update method that doesn't automatically handle mouse input
    // Use this if you want to handle mouse input manually via handleMouseMotion
    void update_without_mouse(float delta)
    {
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            // Call player update but not mouse handling
            // You'll need to implement this in playerEntity if it doesn't exist
            // player_entity->update_player_without_mouse(delta);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            // Manual movement update without automatic mouse handling
            if (!freecam_entity->input_enabled || !freecam_entity->input_manager) return;
            
            // Calculate movement direction in local camera space using direct key queries
            vector3f local_movement = vector3f(0, 0, 0);
            
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_W)) local_movement.Z += 1.0f;  // Forward
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_S)) local_movement.Z -= 1.0f;  // Backward
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_A)) local_movement.X += 1.0f;  // Left
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_D)) local_movement.X -= 1.0f;  // Right
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_SPACE)) local_movement.Y += 1.0f;    // Up
            if (freecam_entity->input_manager->is_key_held(SDL_SCANCODE_LSHIFT)) local_movement.Y -= 1.0f;    // Down
            
            // Normalize movement vector to prevent faster diagonal movement
            if (local_movement.getLength() > 0)
            {
                local_movement = local_movement.normalize();
            }
            
            // Determine current movement speed (using shift for fast movement)
            float current_speed = freecam_entity->input_manager->is_key_held(SDL_SCANCODE_LSHIFT) ? 
                freecam_entity->fast_movement_speed : freecam_entity->movement_speed;
            
            // Transform movement from local camera space to world space
            vector3f world_movement = freecam_entity->freecam_camera.camera_rot_quaternion() * local_movement;
            
            // Apply movement
            freecam_entity->freecam_camera.position += world_movement * current_speed * delta;
            
            // Update the game object position to match camera
            freecam_entity->obj.position = freecam_entity->freecam_camera.position;
        }
    }

    // Get the camera that should be used for rendering
    camera& getActiveCamera()
    {
        if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            return freecam_entity->freecam_camera;
        }
        else if (player_entity)
        {
            return player_entity->player_camera;
        }
        
        // Fallback - should not happen
        static camera fallback_camera;
        return fallback_camera;
    }

    // Getters
    bool isFreecamMode() const { return freecam_mode_enabled; }
    PossessedEntityType getCurrentlyPossessed() const { return currently_possessed; }
    playerEntity* getPlayerEntity() const { return player_entity; }
    freecamEntity* getFreecamEntity() const { return freecam_entity; }
    
    // Mouse sensitivity control - now uses InputManager's sensitivity system
    void setMouseSensitivity(float sensitivity)
    {
        if (input_manager)
        {
            input_manager->Sensitivity_X = sensitivity;
            input_manager->Sensitivity_Y = sensitivity;
        }
    }
    
    void setMouseSensitivityX(float sensitivity)
    {
        if (input_manager)
        {
            input_manager->Sensitivity_X = sensitivity;
        }
    }
    
    void setMouseSensitivityY(float sensitivity)
    {
        if (input_manager)
        {
            input_manager->Sensitivity_Y = sensitivity;
        }
    }
    
    // Set local multipliers for individual entities
    void setPlayerMouseSensitivity(float sensitivity)
    {
        if (player_entity) player_entity->mouse_sensitivity = sensitivity;
    }
    
    void setFreecamMouseSensitivity(float sensitivity)
    {
        if (freecam_entity) freecam_entity->mouse_sensitivity = sensitivity;
    }
    
    // Get InputManager sensitivity
    float getMouseSensitivityX() const
    {
        return input_manager ? input_manager->Sensitivity_X : 1.0f;
    }
    
    float getMouseSensitivityY() const
    {
        return input_manager ? input_manager->Sensitivity_Y : 1.0f;
    }
    
    // Get effective sensitivity for currently possessed entity
    float getEffectiveMouseSensitivityX() const
    {
        if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
            return freecam_entity->get_effective_mouse_sensitivity_x();
        else if (player_entity && input_manager)
            return input_manager->Sensitivity_X * player_entity->mouse_sensitivity;
        return 1.0f;
    }
    
    float getEffectiveMouseSensitivityY() const
    {
        if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
            return freecam_entity->get_effective_mouse_sensitivity_y();
        else if (player_entity && input_manager)
            return input_manager->Sensitivity_Y * player_entity->mouse_sensitivity;
        return 1.0f;
    }
    
    // Get local multiplier for currently possessed entity
    float getLocalMouseSensitivity() const
    {
        if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
            return freecam_entity->mouse_sensitivity;
        else if (player_entity)
            return player_entity->mouse_sensitivity;
        return 1.0f;
    }

private:
    void quit_game()
    {
        printf("Quit requested\n");
        exit(0);
    }
};