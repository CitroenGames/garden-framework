#include "GameFramework/GameState.hpp"

#include "GameFramework/GameFrameworkComponents.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>

namespace GameFramework
{
GameState::GameState() = default;

GameState::~GameState() = default;

void GameState::tick(float delta_time)
{
    GameStateBase::tick(delta_time);

    if (!isMatchInProgress() || !std::isfinite(delta_time) || delta_time <= 0.0f)
        return;

    m_elapsed_accumulator += delta_time;
    while (m_elapsed_accumulator >= 1.0f)
    {
        m_elapsed_accumulator -= 1.0f;
        defaultTimer();
    }
}

void GameState::handleBeginPlay()
{
    GameStateBase::handleBeginPlay();
    syncGameStateComponent();
}

bool GameState::hasMatchStarted() const
{
    return m_match_state != MatchState::EnteringMap &&
        m_match_state != MatchState::WaitingToStart;
}

bool GameState::hasMatchEnded() const
{
    return m_match_state == MatchState::WaitingPostMatch ||
        m_match_state == MatchState::LeavingMap;
}

float GameState::getPlayerStartTime(uint16_t player_id) const
{
    (void)player_id;
    return static_cast<float>(m_elapsed_time);
}

float GameState::getPlayerRespawnDelay(uint16_t player_id) const
{
    return GameStateBase::getPlayerRespawnDelay(player_id);
}

bool GameState::isMatchInProgress() const
{
    return m_match_state == MatchState::InProgress;
}

void GameState::setMatchState(const std::string& new_state)
{
    if (new_state.empty() || new_state == m_match_state)
        return;

    m_previous_match_state = m_match_state;
    m_match_state = new_state;
    onMatchStateSet();
    syncGameStateComponent();
}

void GameState::defaultTimer()
{
    if (isMatchInProgress())
    {
        ++m_elapsed_time;
        syncGameStateComponent();
    }
}

void GameState::onMatchStateSet()
{
    if (m_match_state == MatchState::WaitingToStart ||
        m_previous_match_state == MatchState::EnteringMap)
        handleMatchIsWaitingToStart();

    if (m_match_state == MatchState::InProgress)
        handleMatchHasStarted();
    else if (m_match_state == MatchState::WaitingPostMatch)
        handleMatchHasEnded();
    else if (m_match_state == MatchState::LeavingMap)
        handleLeavingMap();
    else if (m_match_state == MatchState::Aborted)
        handleMatchAborted();
}

void GameState::handleMatchIsWaitingToStart()
{
    m_elapsed_time = 0;
    m_elapsed_accumulator = 0.0f;
}

void GameState::handleMatchHasStarted()
{
    m_elapsed_time = 0;
    m_elapsed_accumulator = 0.0f;
}

void GameState::handleMatchHasEnded()
{
}

void GameState::handleLeavingMap()
{
}

void GameState::handleMatchAborted()
{
}

void GameState::syncGameStateComponent()
{
    GameStateBase::syncGameStateComponent();

    if (!m_world)
        return;

    const entt::entity entity = getOrCreateGameStateEntity(m_world->registry);
    auto& component = m_world->registry.get_or_emplace<GameStateComponent>(entity);
    component.match_state = m_match_state;
    component.previous_match_state = m_previous_match_state;
    component.match_started = hasMatchStarted();
    component.match_ended = hasMatchEnded();
    component.elapsed_time = m_elapsed_time;
}
}
