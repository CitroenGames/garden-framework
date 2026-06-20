#include "GameFramework/GameStateBase.hpp"

#include "GameFramework/GameModeBase.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameFramework
{
GameStateBase::GameStateBase() = default;

GameStateBase::~GameStateBase() = default;

void GameStateBase::initialize(world* game_world)
{
    m_world = game_world;
    m_has_begun_play = false;
    m_server_world_time_seconds = 0.0;
    m_server_time_update_accumulator = 0.0f;
}

void GameStateBase::shutdown()
{
    m_player_array.clear();
    m_authority_game_mode = nullptr;
    m_world = nullptr;
    m_has_begun_play = false;
    m_server_world_time_seconds = 0.0;
    m_server_time_update_accumulator = 0.0f;
}

void GameStateBase::tick(float delta_time)
{
    if (!std::isfinite(delta_time) || delta_time <= 0.0f)
        return;

    m_server_world_time_seconds += static_cast<double>(delta_time);
    if (m_server_time_update_frequency > 0.0f)
    {
        m_server_time_update_accumulator += delta_time;
        if (m_server_time_update_accumulator >= m_server_time_update_frequency)
            m_server_time_update_accumulator = 0.0f;
    }
}

void GameStateBase::setGameModeClassName(std::string class_name)
{
    m_game_mode_class_name = std::move(class_name);
    receivedGameModeClass();
}

void GameStateBase::setAuthorityGameMode(GameModeBase* game_mode)
{
    m_authority_game_mode = game_mode;
}

void GameStateBase::setSpectatorClassName(std::string class_name)
{
    m_spectator_class_name = std::move(class_name);
    receivedSpectatorClass();
}

void GameStateBase::addPlayerState(PlayerStatePtr player_state)
{
    if (!player_state)
        return;

    const uint16_t player_id = player_state->player_id;
    auto it = std::find_if(m_player_array.begin(), m_player_array.end(),
        [player_id](const PlayerStatePtr& existing) {
            return existing && existing->player_id == player_id;
        });

    if (it != m_player_array.end())
    {
        *it = std::move(player_state);
        return;
    }

    m_player_array.push_back(std::move(player_state));
}

void GameStateBase::removePlayerState(uint16_t player_id)
{
    m_player_array.erase(
        std::remove_if(m_player_array.begin(), m_player_array.end(),
            [player_id](const PlayerStatePtr& player_state) {
                return !player_state || player_state->player_id == player_id;
            }),
        m_player_array.end());
}

PlayerState* GameStateBase::getPlayerState(uint16_t player_id) const
{
    for (const PlayerStatePtr& player_state : m_player_array)
    {
        if (player_state && player_state->player_id == player_id)
            return player_state.get();
    }
    return nullptr;
}

PlayerState* GameStateBase::getPlayerStateByName(const std::string& player_name) const
{
    for (const PlayerStatePtr& player_state : m_player_array)
    {
        if (player_state && player_state->player_name == player_name)
            return player_state.get();
    }
    return nullptr;
}

void GameStateBase::handleBeginPlay()
{
    m_has_begun_play = true;
}

float GameStateBase::getPlayerStartTime(uint16_t player_id) const
{
    const PlayerState* player_state = getPlayerState(player_id);
    return player_state ? player_state->start_time : 0.0f;
}

float GameStateBase::getPlayerRespawnDelay(uint16_t player_id) const
{
    return m_authority_game_mode ? m_authority_game_mode->getPlayerRespawnDelay(player_id) : 0.0f;
}

void GameStateBase::setServerWorldTimeSecondsUpdateFrequency(float frequency)
{
    m_server_time_update_frequency = std::isfinite(frequency) ? std::max(frequency, 0.0f) : 0.0f;
    m_server_time_update_accumulator = 0.0f;
}
}
