#include "GameSimulation.hpp"
#include "PlayerRepSystem.hpp"
#include "Events/EventBus.hpp"
#include "Timer/TimerSystem.hpp"
#include "Debug/DebugDraw.hpp"
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

    // Find player representation entity
    {
        auto view = m_world->registry.view<PlayerRepresentationComponent>();
        for (auto entity : view)
        {
            m_player_rep_entity = entity;
            break;
        }
    }

    // Optimize broad phase after all bodies are set up
    m_world->getPhysicsSystem().getJoltSystem()->OptimizeBroadPhase();

    m_initialized = true;

    LOG_ENGINE_INFO("GameSimulation initialized (player={}, freecam={})",
        m_world->registry.valid(m_player_entity),
        m_world->registry.valid(m_freecam_entity));
}

void GameSimulation::update(float delta_time)
{
    if (!m_initialized || m_paused)
        return;

    // Flush deferred events from previous frame
    EventBus::get().flush();

    // Update timers
    TimerSystem::get().update(delta_time);

    PlayerController* player_controller = getPlayerController();

    // Physics and player collisions (only when controlling player, not freecam)
    if (player_controller && !player_controller->isFreecamMode())
    {
        m_world->step_physics(delta_time);

        if (m_world->registry.valid(m_player_entity))
            m_world->player_collisions(m_player_entity);
    }

    // Update player controller (movement from input)
    if (player_controller)
        player_controller->update(delta_time);

    // Update player representations
    bool is_freecam = player_controller ? player_controller->isFreecamMode() : false;
    update_player_representations(m_world->registry, is_freecam);

    // Update animations
    AnimationSystem::update(m_world->registry, delta_time);

    // Update audio listener to match active camera
    camera& active_cam = getActiveCamera();
    AudioSystem::get().setListenerPosition(
        active_cam.getPosition(),
        active_cam.camera_forward(),
        active_cam.getUpVector());
    AudioSystem::get().update();
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
