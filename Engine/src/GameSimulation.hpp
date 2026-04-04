#pragma once

#include "EngineExport.h"
#include "world.hpp"
#include "InputManager.hpp"
#include "PlayerController.hpp"
#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include <memory>
#include <entt/entt.hpp>

// Encapsulates one frame of game-logic: physics, player controller,
// animation, audio, timers, events, debug draw.
// Does NOT own the world -- operates on one passed to it.
// Does NOT handle rendering or window management.
class ENGINE_API GameSimulation
{
public:
    GameSimulation(world* game_world, std::shared_ptr<InputManager> input_mgr);
    ~GameSimulation();

    // Find player/freecam entities in the registry and create PlayerController.
    // Call once after the world is populated with entities.
    void initialize();

    // Run one frame of game logic.
    void update(float delta_time);

    // Forward mouse motion to the PlayerController.
    void handleMouseMotion(float mouse_dy, float mouse_dx);

    void setPaused(bool paused);
    bool isPaused() const;

    // Camera that should be used for rendering during play.
    camera& getActiveCamera();

    PlayerController* getPlayerController();

    entt::entity getPlayerEntity() const  { return m_player_entity; }
    entt::entity getFreecamEntity() const { return m_freecam_entity; }

private:
    world*                          m_world;
    std::shared_ptr<InputManager>   m_input_manager;
    std::unique_ptr<PlayerController> m_player_controller;

    entt::entity m_player_entity     = entt::null;
    entt::entity m_freecam_entity    = entt::null;
    entt::entity m_player_rep_entity = entt::null;

    bool m_paused      = false;
    bool m_initialized = false;
};
