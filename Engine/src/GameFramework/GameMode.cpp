#include "GameFramework/GameMode.hpp"

#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "GameFramework/GameState.hpp"
#include "Utils/Log.hpp"

#include <algorithm>
#include <cmath>

namespace GameFramework
{
GameMode::GameMode() = default;

GameMode::~GameMode() = default;

std::unique_ptr<GameStateBase> GameMode::createGameState()
{
    return std::make_unique<GameState>();
}

void GameMode::initGame(world& game_world,
                        const std::string& map_name,
                        const std::string& options,
                        std::string& error_message)
{
    GameModeBase::initGame(game_world, map_name, options, error_message);
    setMatchState(MatchState::WaitingToStart);
}

void GameMode::startPlay()
{
    GameModeBase::startPlay();

    if (m_match_state == MatchState::EnteringMap)
        setMatchState(MatchState::WaitingToStart);

    if (m_match_state == MatchState::WaitingToStart && readyToStartMatch())
        startMatch();
}

void GameMode::tick(float delta_time)
{
    GameModeBase::tick(delta_time);

    if (m_match_state == MatchState::WaitingToStart && readyToStartMatch())
        startMatch();
    else if (m_match_state == MatchState::InProgress && readyToEndMatch())
        endMatch();
}

bool GameMode::hasMatchStarted() const
{
    return MatchState::order(m_match_state) >= MatchState::order(MatchState::InProgress);
}

bool GameMode::hasMatchEnded() const
{
    return MatchState::order(m_match_state) >= MatchState::order(MatchState::WaitingPostMatch);
}

void GameMode::postLogin(PlayerControllerEntry& new_player)
{
    GameModeBase::postLogin(new_player);

    if (m_match_state == MatchState::WaitingToStart && readyToStartMatch())
        startMatch();
}

void GameMode::logout(uint16_t player_id)
{
    GameModeBase::logout(player_id);

    if (m_match_state == MatchState::InProgress && readyToEndMatch())
        endMatch();
}

bool GameMode::playerCanRestart(const PlayerControllerEntry& player) const
{
    return GameModeBase::playerCanRestart(player) && !hasMatchEnded();
}

float GameMode::getPlayerRespawnDelay(uint16_t player_id) const
{
    (void)player_id;
    return m_min_respawn_delay;
}

bool GameMode::isMatchInProgress() const
{
    return m_match_state == MatchState::InProgress;
}

void GameMode::startMatch()
{
    if (m_match_state == MatchState::InProgress || hasMatchEnded())
        return;

    setMatchState(MatchState::InProgress);
}

void GameMode::endMatch()
{
    if (hasMatchEnded())
        return;

    setMatchState(MatchState::WaitingPostMatch);
}

void GameMode::restartGame()
{
    setMatchState(MatchState::WaitingToStart);
    for (PlayerControllerEntry& player : m_players)
    {
        if (!player.spectator)
            restartPlayer(player);
    }

    if (readyToStartMatch())
        startMatch();
}

void GameMode::abortMatch()
{
    setMatchState(MatchState::Aborted);
}

void GameMode::startToLeaveMap()
{
    setMatchState(MatchState::LeavingMap);
}

void GameMode::setMinRespawnDelay(float delay)
{
    m_min_respawn_delay = std::isfinite(delay) ? std::max(delay, 0.0f) : 0.0f;
}

void GameMode::setMatchState(const std::string& new_state)
{
    if (new_state.empty() || new_state == m_match_state)
        return;

    const std::string old_state = m_match_state;
    m_match_state = new_state;

    LOG_ENGINE_INFO("MatchState: {} -> {}", old_state, m_match_state);
    onMatchStateSet();

    if (auto* state = dynamic_cast<GameState*>(m_game_state))
        state->setMatchState(m_match_state);

    EventBus::get().queue(MatchStateChangedEvent{old_state, m_match_state});
}

void GameMode::onMatchStateSet()
{
    if (m_match_state == MatchState::WaitingToStart)
        handleMatchIsWaitingToStart();
    else if (m_match_state == MatchState::InProgress)
        handleMatchHasStarted();
    else if (m_match_state == MatchState::WaitingPostMatch)
        handleMatchHasEnded();
    else if (m_match_state == MatchState::LeavingMap)
        handleLeavingMap();
    else if (m_match_state == MatchState::Aborted)
        handleMatchAborted();
}

void GameMode::handleMatchIsWaitingToStart()
{
}

bool GameMode::readyToStartMatch() const
{
    return !m_delayed_start && getNumPlayers() > 0;
}

void GameMode::handleMatchHasStarted()
{
    if (m_game_state && !m_game_state->hasBegunPlay())
        m_game_state->handleBeginPlay();
}

bool GameMode::readyToEndMatch() const
{
    return false;
}

void GameMode::handleMatchHasEnded()
{
}

void GameMode::handleLeavingMap()
{
}

void GameMode::handleMatchAborted()
{
}
}
