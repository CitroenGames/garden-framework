#include "GameSimulation.hpp"
#include "PlayerRepSystem.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "Timer/TimerSystem.hpp"
#include "Audio/AudioSystem.hpp"
#include "Animation/AnimationSystem.hpp"
#include "GameFramework/GameMode.hpp"
#include "Utils/Log.hpp"

GameSimulation::GameSimulation(world* game_world, std::shared_ptr<InputManager> input_mgr)
    : m_world(game_world)
    , m_input_manager(std::move(input_mgr))
{
}

GameSimulation::~GameSimulation()
{
    m_game_mode = nullptr;
}

void GameSimulation::registerSystems()
{
    // engine.timers is auto-registered by the scheduler in Gameplay (fixed dt).
    {
        SystemDescriptor d;
        d.name = "engine.physics";
        d.phase = Phase::Physics;
        d.update = [this](entt::registry&, const Tick::SimulationTickContext&) {
            // Never gated on player/freecam state (D5 rule 6).
            m_world->stepPhysicsSingleStep();
        };
        m_scheduler->addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.triggers";
        d.phase = Phase::Physics;
        d.run_after = {"engine.physics"};
        d.update = [this](entt::registry& reg, const Tick::SimulationTickContext&) {
            m_world->getPhysicsSystem().updateTriggers(reg);
        };
        m_scheduler->addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.player_collisions";
        d.phase = Phase::Physics;
        d.run_after = {"engine.physics"};
        d.update = [this](entt::registry&, const Tick::SimulationTickContext&) {
            if (m_world->registry.valid(m_player_entity))
                m_world->player_collisions(m_player_entity);
        };
        m_scheduler->addSystem(std::move(d));
    }
    // PlayerController::update is not split (D5 rule 5): whole call runs
    // in Presentation-adjacent offline handling with variable frame dt.
    {
        SystemDescriptor d;
        d.name = "engine.player_controller";
        d.phase = Phase::Presentation;
        d.update = [this](entt::registry&, const Tick::SimulationTickContext&) {
            if (PlayerController* pc = getPlayerController())
                pc->update(m_current_frame_dt);
        };
        m_scheduler->addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.player_representation";
        d.phase = Phase::Presentation;
        d.run_after = {"engine.player_controller"};
        d.update = [this](entt::registry& reg, const Tick::SimulationTickContext&) {
            PlayerController* pc = getPlayerController();
            update_player_representations(reg, pc ? pc->isFreecamMode() : false);
        };
        m_scheduler->addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.animation";
        d.phase = Phase::Presentation;
        d.run_after = {"engine.player_representation"};
        d.update = [this](entt::registry& reg, const Tick::SimulationTickContext&) {
            AnimationSystem::update(reg, m_current_frame_dt);
        };
        m_scheduler->addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.audio";
        d.phase = Phase::Presentation;
        d.run_after = {"engine.animation"};
        d.update = [this](entt::registry&, const Tick::SimulationTickContext&) {
            camera& cam = getActiveCamera();
            AudioSystem::get().setListenerPosition(
                cam.getPosition(), cam.camera_forward(), cam.getUpVector());
            AudioSystem::get().update();
        };
        m_scheduler->addSystem(std::move(d));
    }
}

void GameSimulation::initialize()
{
    if (!m_world)
    {
        LOG_ENGINE_ERROR("GameSimulation::initialize() called with null world");
        return;
    }

    if (!m_world->getAuthorityGameMode())
        m_world->setAuthorityGameMode(std::make_unique<GameFramework::GameMode>());

    m_game_mode = m_world->getAuthorityGameMode();
    m_game_mode->setInputManager(m_input_manager);

    std::string error_message;
    if (!m_world->initializeGameplayFramework("", ""))
        LOG_ENGINE_WARN("Gameplay framework did not initialize");

    if (!m_game_mode->getPrimaryPlayer())
    {
        GameFramework::PlayerLoginOptions login_options;
        login_options.player_id = 1;
        login_options.player_name = "Player";
        m_game_mode->createLocalPlayer(login_options, error_message);
        if (!error_message.empty())
            LOG_ENGINE_WARN("Local player login failed: {}", error_message);
    }

    m_world->startGameplayFramework();

    if (const GameFramework::PlayerControllerEntry* primary_player = m_game_mode->getPrimaryPlayer())
    {
        m_player_entity = primary_player->pawn;
        m_freecam_entity = primary_player->freecam;
    }

    {
        auto view = m_world->registry.view<PlayerRepresentationComponent>();
        for (auto entity : view)
        {
            m_player_rep_entity = entity;
            break;
        }
    }

    m_world->getPhysicsSystem().getJoltSystem()->OptimizeBroadPhase();

    m_lifecycle = std::make_unique<EntityLifecycle>(m_world->registry);
    m_scheduler = std::make_unique<SystemScheduler>(false, m_world->getFixedDelta());
    m_scheduler->setLifecycle(m_lifecycle.get());
    m_scheduler->setPaused(m_paused);
    registerSystems();

    m_initialized = true;

    LOG_ENGINE_INFO("GameSimulation initialized (player={}, freecam={})",
        m_world->registry.valid(m_player_entity),
        m_world->registry.valid(m_freecam_entity));
}

void GameSimulation::update(float delta_time)
{
    if (!m_initialized || !m_scheduler)
        return;
    m_current_frame_dt = delta_time;
    m_scheduler->runFrame(m_world->registry, delta_time);
}

void GameSimulation::handleMouseMotion(float mouse_dy, float mouse_dx)
{
    PlayerController* player_controller = getPlayerController();
    if (player_controller && m_initialized && !m_paused)
        player_controller->handleMouseMotion(mouse_dy, mouse_dx);
}

void GameSimulation::setPaused(bool paused)
{
    m_paused = paused;
    if (m_scheduler) m_scheduler->setPaused(paused);
    if (m_game_mode)
    {
        if (paused)
            m_game_mode->setPause();
        else
            m_game_mode->clearPause();
    }
}

bool GameSimulation::isPaused() const
{
    return m_paused;
}

camera& GameSimulation::getActiveCamera()
{
    PlayerController* player_controller = getPlayerController();
    if (player_controller)
        return player_controller->getActiveCamera();
    return m_world->world_camera;
}

PlayerController* GameSimulation::getPlayerController()
{
    return m_game_mode ? m_game_mode->getPrimaryPlayerController() : nullptr;
}
