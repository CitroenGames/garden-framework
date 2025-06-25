#pragma once

#include "gameObject.hpp"
#include "camera.hpp"
#include "playerEntity.hpp"
#include "freecamEntity.hpp"

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
    PossessedEntityType currently_possessed;
    bool freecam_mode_enabled;

public:
    PlayerController() 
        : player_entity(nullptr), freecam_entity(nullptr), 
          currently_possessed(PossessedEntityType::Player), 
          freecam_mode_enabled(false) 
    {
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
            printf("Switched to player mode\n");
        }
    }

    // Input handling - only pass to currently possessed entity
    void handleMouseMotion(float yrel, float xrel)
    {
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            player_entity->update_camera(yrel, xrel);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->update_camera(yrel, xrel);
        }
    }

    void handleKeyDown(SDL_Keysym* keysym)
    {
        // Handle freecam toggle (F key)
        if (keysym->sym == SDLK_f)
        {
            toggleFreecamMode();
            return;
        }

        // Pass input to currently possessed entity
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            player_entity->handle_input_down(keysym);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->handle_input_down(keysym);
        }
    }

    void handleKeyUp(SDL_Keysym* keysym)
    {
        // Pass input to currently possessed entity
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            player_entity->handle_input_up(keysym);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->handle_input_up(keysym);
        }
    }

    // Update currently possessed entity
    void update(float delta)
    {
        if (currently_possessed == PossessedEntityType::Player && player_entity)
        {
            player_entity->update_player(delta);
        }
        else if (currently_possessed == PossessedEntityType::Freecam && freecam_entity)
        {
            freecam_entity->update_freecam(delta);
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
};