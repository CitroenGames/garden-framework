#include "GameSimulation.hpp"
#include "PlayerRepSystem.hpp"
#include "Events/EventBus.hpp"
#include "Timer/TimerSystem.hpp"
#include "Debug/DebugDraw.hpp"
#include "Audio/AudioSystem.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Utils/Log.hpp"

GameSimulation::GameSimulation(world* game_world, std::shared_ptr<InputManager> input_mgr)
    : m_world(game_world)
    , m_input_manager(std::move(input_mgr))
{
}

GameSimulation::~GameSimulation()
{
    m_player_controller.reset();
}

void GameSimulation::initialize()
{
    if (!m_world)
    {
        LOG_ENGINE_ERROR("GameSimulation::initialize() called with null world");
        return;
    }

    // Find player entity
    {
        auto view = m_world->registry.view<PlayerComponent>();
        for (auto entity : view)
        {
            m_player_entity = entity;
            break; // use first player found
        }
    }

    // Find freecam entity
    {
        auto view = m_world->registry.view<FreecamComponent>();
        for (auto entity : view)
        {
            m_freecam_entity = entity;
            break;
        }
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

    // Create PlayerController
    m_player_controller = std::make_unique<PlayerController>(m_input_manager, m_world);

    if (m_world->registry.valid(m_player_entity))
    {
        m_player_controller->setPossessedPlayer(m_player_entity);

        // Sync world camera to player position
        auto& t = m_world->registry.get<TransformComponent>(m_player_entity);
        m_world->world_camera.position = t.position;
        m_world->world_camera.rotation = t.rotation;
    }

    if (m_world->registry.valid(m_freecam_entity))
    {
        m_player_controller->setPossessedFreecam(m_freecam_entity);
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

    // Physics and player collisions (only when controlling player, not freecam)
    if (m_player_controller && !m_player_controller->isFreecamMode())
    {
        m_world->step_physics(delta_time);

        if (m_world->registry.valid(m_player_entity))
            m_world->player_collisions(m_player_entity);
    }

    // Update player controller (movement from input)
    if (m_player_controller)
        m_player_controller->update(delta_time);

    // Update player representations
    bool is_freecam = m_player_controller ? m_player_controller->isFreecamMode() : false;
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
    if (m_player_controller && m_initialized && !m_paused)
        m_player_controller->handleMouseMotion(mouse_dy, mouse_dx);
}

void GameSimulation::setPaused(bool paused)
{
    m_paused = paused;
}

bool GameSimulation::isPaused() const
{
    return m_paused;
}

camera& GameSimulation::getActiveCamera()
{
    if (m_player_controller)
        return m_player_controller->getActiveCamera();
    return m_world->world_camera;
}

PlayerController* GameSimulation::getPlayerController()
{
    return m_player_controller.get();
}
