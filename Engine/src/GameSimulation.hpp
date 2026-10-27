#pragma once

#include "EngineExport.h"
#include "Components/LocalPlayerInputComponent.hpp"
#include "world.hpp"
#include "InputManager.hpp"
#include "PlayerController.hpp"
#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "Scheduler/SystemScheduler.hpp"
#include <memory>
#include <string>
#include <entt/entt.hpp>

namespace GameFramework
{
    class GameModeBase;
}

class EntityLifecycle;
class InputActionSystem;

// Thin scheduler host (D5: retired as loop owner). Owns a SystemScheduler
// and an EntityLifecycle for the world; update() delegates to
// SystemScheduler::runFrame. The hand-ordered loop body is gone.
// Client (Game.exe) and dedicated server do NOT use this class: they own
// their own schedulers (server: Game/Server/src/main.cpp). This host is
// only the editor standalone/legacy path.
class ENGINE_API GameSimulation
{
public:
    GameSimulation(world* game_world,
                   std::shared_ptr<InputManager> input_mgr,
                   InputActionSystem* input_actions,
                   LocalPlayerInputActionNames local_input_actions,
                   std::string local_input_context);
    ~GameSimulation();

    void initialize();
    void update(float delta_time);
    void handleMouseMotion(float mouse_dy, float mouse_dx);
    void setPaused(bool paused);
    bool isPaused() const;
    camera& getActiveCamera();
    PlayerController* getPlayerController();
    entt::entity getPlayerEntity() const  { return m_player_entity; }
    entt::entity getFreecamEntity() const { return m_freecam_entity; }
    SystemScheduler* getScheduler() { return m_scheduler.get(); }

private:
    void registerSystems();

    world*                          m_world;
    std::shared_ptr<InputManager>   m_input_manager;
    InputActionSystem*              m_input_actions = nullptr;
    LocalPlayerInputActionNames     m_local_input_actions;
    std::string                     m_local_input_context;
    GameFramework::GameModeBase*    m_game_mode = nullptr;
    std::unique_ptr<SystemScheduler> m_scheduler;
    std::unique_ptr<EntityLifecycle> m_lifecycle;
    float m_current_frame_dt = 0.0f;

    entt::entity m_player_entity     = entt::null;
    entt::entity m_freecam_entity    = entt::null;
    entt::entity m_player_rep_entity = entt::null;

    bool m_paused      = false;
    bool m_initialized = false;
};
