#include "GameFramework/GameFrameworkRuntime.hpp"

#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "Utils/Log.hpp"

#include <utility>

namespace GameFramework
{
GameFrameworkRuntime::GameFrameworkRuntime() = default;
GameFrameworkRuntime::~GameFrameworkRuntime() = default;

void GameFrameworkRuntime::setAuthorityGameMode(world& game_world, std::unique_ptr<GameModeBase> mode)
{
    m_mode = std::move(mode);
    if (m_mode)
        m_mode->bindWorld(game_world);
}

void GameFrameworkRuntime::setGameState(std::unique_ptr<GameStateBase> state)
{
    m_state = std::move(state);
}

void GameFrameworkRuntime::rebindMovedWorld(world& previous, world& current)
{
    if (m_mode)
        m_mode->rebindMovedWorld(previous, current);
    if (m_state && m_state->m_world == &previous)
        m_state->m_world = &current;
}

void GameFrameworkRuntime::clear()
{
    shutdown();
    m_mode.reset();
    m_state.reset();
}

bool GameFrameworkRuntime::initialize(world& game_world, const std::string& map_name, const std::string& options)
{
    if (m_initialized)
        return true;
    if (m_mode)
    {
        std::string error_message;
        m_mode->initGame(game_world, map_name, options, error_message);
        if (!error_message.empty())
        {
            LOG_ENGINE_WARN("GameMode init reported: {}", error_message);
            return false;
        }
        m_initialized = true;
        return true;
    }
    if (m_state)
    {
        m_state->initialize(&game_world);
        m_state->setAuthorityGameMode(nullptr);
        m_initialized = true;
        return true;
    }
    return false;
}

void GameFrameworkRuntime::start(world& game_world)
{
    if (!m_initialized && !initialize(game_world))
        return;
    if (m_started)
        return;
    if (m_mode)
        m_mode->startPlay();
    else if (m_state)
        m_state->handleBeginPlay();
    m_started = true;
}

void GameFrameworkRuntime::tick(world& game_world, float delta_time)
{
    if (!m_initialized && !initialize(game_world))
        return;
    if (m_mode)
        m_mode->tick(delta_time);
    else if (m_state)
        m_state->tick(delta_time);
}

void GameFrameworkRuntime::shutdown()
{
    if (m_state && m_initialized)
        m_state->shutdown();
    m_initialized = false;
    m_started = false;
}
}
