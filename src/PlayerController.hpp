#pragma once

#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "InputManager.hpp"
#include "world.hpp"
#include <entt/entt.hpp>
#include <cmath>

enum class PossessedEntityType
{
    Player,
    Freecam
};

class PlayerController
{
private:
    entt::entity player_entity;
    entt::entity freecam_entity;
    std::shared_ptr<InputManager> input_manager;
    PossessedEntityType currently_possessed;
    bool freecam_mode_enabled;
    world* game_world;

public:
    PlayerController(std::shared_ptr<InputManager> input_mgr, world* w) 
        : input_manager(input_mgr), game_world(w),
          player_entity(entt::null), freecam_entity(entt::null), 
          currently_possessed(PossessedEntityType::Player), 
          freecam_mode_enabled(false) 
    {
        setup_input_bindings();
    }

    void setup_input_bindings()
    {
        if (!input_manager) return; 
        
        input_manager->bind_action("ToggleFreecam", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                toggleFreecamMode();
            }
        });
        
        input_manager->bind_action("Quit", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                exit(0);
            }
        });
    }

    void setPossessedPlayer(entt::entity player)
    {
        player_entity = player;
    }

    void setPossessedFreecam(entt::entity freecam)
    {
        freecam_entity = freecam;
    }

    void toggleFreecamMode()
    {
        if (!game_world->registry.valid(player_entity) || !game_world->registry.valid(freecam_entity))
            return;

        freecam_mode_enabled = !freecam_mode_enabled;

        if (freecam_mode_enabled)
        {
            currently_possessed = PossessedEntityType::Freecam;
            
            // Sync freecam to player position
            auto& p_trans = game_world->registry.get<TransformComponent>(player_entity);
            auto& f_trans = game_world->registry.get<TransformComponent>(freecam_entity);
            
            f_trans.position = p_trans.position;
            // Also sync camera rotation (stored in world camera, but we want freecam to start looking same way)
            // But freecam entity doesn't store camera rotation usually, it uses the camera directly.
            // We'll just rely on the world camera state being preserved.
            
            printf("Switched to freecam mode\n");
        }
        else
        {
            currently_possessed = PossessedEntityType::Player;
            printf("Switched to player mode\n");
        }
    }

    void handleMouseMotion(float yrel, float xrel)
    {
        if (!game_world) return; 
        
        camera& active_cam = game_world->world_camera;
        float sensitivity = 1.0f;

        if (currently_possessed == PossessedEntityType::Player && game_world->registry.valid(player_entity))
        {
             auto& pc = game_world->registry.get<PlayerComponent>(player_entity);
             sensitivity = pc.mouse_sensitivity;
        }
        else if (currently_possessed == PossessedEntityType::Freecam && game_world->registry.valid(freecam_entity))
        {
             auto& fc = game_world->registry.get<FreecamComponent>(freecam_entity);
             sensitivity = fc.mouse_sensitivity;
        }

        float effective_sensitivity_x = input_manager ? input_manager->Sensitivity_X * sensitivity : sensitivity;
        float effective_sensitivity_y = input_manager ? input_manager->Sensitivity_Y * sensitivity : sensitivity;

        active_cam.rotation.X += yrel / 1000.0f * effective_sensitivity_y;
        active_cam.rotation.Y += -xrel / 1000.0f * effective_sensitivity_x;
        
        // Clamp pitch
        if (active_cam.rotation.X > 1.5f) active_cam.rotation.X = 1.5f;
        if (active_cam.rotation.X < -1.5f) active_cam.rotation.X = -1.5f;
    }

    void update(float delta)
    {
        if (!game_world) return; 

        if (currently_possessed == PossessedEntityType::Player)
        {
            updatePlayer(delta);
        }
        else if (currently_possessed == PossessedEntityType::Freecam)
        {
            updateFreecam(delta);
        }
    }

private:
    void updatePlayer(float delta)
    {
        if (!game_world->registry.valid(player_entity)) return; 

        auto& pc = game_world->registry.get<PlayerComponent>(player_entity);
        auto& rb = game_world->registry.get<RigidBodyComponent>(player_entity);
        auto& trans = game_world->registry.get<TransformComponent>(player_entity);
        
        if (!pc.input_enabled || !input_manager) return; 

        float move_forward = 0.0f;
        float move_right = 0.0f;

        if (input_manager->is_key_held(SDL_SCANCODE_W)) move_forward += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) move_forward -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) move_right -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) move_right += 1.0f;

        vector3f wish_dir = vector3f(move_right, 0, move_forward);
        
        // Rotate wish_dir by camera
        wish_dir = game_world->world_camera.camera_rot_quaternion() * wish_dir;

        if (pc.grounded)
        {
            wish_dir.projectOnPlane(pc.ground_normal);
        }
        else
        {
            wish_dir.Y = 0;
        }

        wish_dir = wish_dir.normalize();

        bool wishJump = input_manager->is_key_held(SDL_SCANCODE_SPACE);
        bool jump = wishJump && pc.grounded;

        rb.velocity += wish_dir * pc.speed * delta;
        
        if (pc.grounded)
        {
            rb.velocity *= 0.6f; // Friction
            if (jump)
                rb.velocity.Y = pc.jump_force;
            else
                rb.velocity.Y = 0; // Stick to surfaces
        }
        else
        {
            rb.velocity *= 0.7f; // Air friction
            rb.velocity.Y -= 2.0f * delta; // Extra gravity? PhysicsSystem already applies gravity.
            // Original code had `velocity.Y -= 2.0f * delta`. PhysicsSystem adds `gravity * delta`.
            // If gravity is (0, -1, 0), it adds -1*delta.
            // This extra -2*delta makes it fall faster?
            // I'll keep it to match original feel.
        }

        // Camera follow
        // Interpolate camera position to player position
        game_world->world_camera.position = trans.position.getInterpolated(game_world->world_camera.position, delta);
    }

    void updateFreecam(float delta)
    {
        if (!game_world->registry.valid(freecam_entity)) return; 

        auto& fc = game_world->registry.get<FreecamComponent>(freecam_entity);
        auto& trans = game_world->registry.get<TransformComponent>(freecam_entity);

        if (!fc.input_enabled || !input_manager) return; 

        vector3f local_movement = vector3f(0, 0, 0);
        
        if (input_manager->is_key_held(SDL_SCANCODE_W)) local_movement.Z += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_S)) local_movement.Z -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_A)) local_movement.X += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_D)) local_movement.X -= 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) local_movement.Y += 1.0f;
        if (input_manager->is_key_held(SDL_SCANCODE_LSHIFT)) local_movement.Y -= 1.0f;

        if (local_movement.getLength() > 0)
            local_movement = local_movement.normalize();

        float current_speed = input_manager->is_key_held(SDL_SCANCODE_LSHIFT) ? fc.fast_movement_speed : fc.movement_speed;

        vector3f world_movement = game_world->world_camera.camera_rot_quaternion() * local_movement;

        // Move camera directly (freecam mode moves the camera)
        game_world->world_camera.position += world_movement * current_speed * delta;
        
        // Sync entity transform to camera
        trans.position = game_world->world_camera.position;
    }

public:
    // Get active camera (always world camera now, as we update it)
    camera& getActiveCamera()
    {
        return game_world->world_camera;
    }
    
    bool isFreecamMode() const { return freecam_mode_enabled; }
};
