#pragma once

#include "EngineExport.h"
#include "GameFramework/PlayerState.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class world;

namespace GameFramework
{
class GameModeBase;

class ENGINE_API GameStateBase
{
public:
    GameStateBase();
    virtual ~GameStateBase();

    GameStateBase(const GameStateBase&) = delete;
    GameStateBase& operator=(const GameStateBase&) = delete;

    virtual const char* getClassName() const { return "GameStateBase"; }

    virtual void initialize(world* game_world);
    virtual void shutdown();
    virtual void tick(float delta_time);

    world* getWorld() const { return m_world; }

    void setGameModeClassName(std::string class_name);
    const std::string& getGameModeClassName() const { return m_game_mode_class_name; }

    void setAuthorityGameMode(GameModeBase* game_mode);
    GameModeBase* getAuthorityGameMode() const { return m_authority_game_mode; }

    void setSpectatorClassName(std::string class_name);
    const std::string& getSpectatorClassName() const { return m_spectator_class_name; }

    virtual void receivedGameModeClass() {}
    virtual void receivedSpectatorClass() {}

    virtual void addPlayerState(PlayerStatePtr player_state);
    virtual void removePlayerState(uint16_t player_id);
    PlayerState* getPlayerState(uint16_t player_id) const;
    PlayerState* getPlayerStateByName(const std::string& player_name) const;
    const std::vector<PlayerStatePtr>& getPlayerArray() const { return m_player_array; }

    virtual void handleBeginPlay();
    virtual bool hasBegunPlay() const { return m_has_begun_play; }
    virtual bool hasMatchStarted() const { return m_has_begun_play; }
    virtual bool hasMatchEnded() const { return false; }

    virtual double getServerWorldTimeSeconds() const { return m_server_world_time_seconds; }
    virtual float getPlayerStartTime(uint16_t player_id) const;
    virtual float getPlayerRespawnDelay(uint16_t player_id) const;

    void setServerWorldTimeSecondsUpdateFrequency(float frequency);
    float getServerWorldTimeSecondsUpdateFrequency() const { return m_server_time_update_frequency; }

protected:
    world* m_world = nullptr;
    GameModeBase* m_authority_game_mode = nullptr;
    std::string m_game_mode_class_name;
    std::string m_spectator_class_name;
    std::vector<PlayerStatePtr> m_player_array;
    bool m_has_begun_play = false;
    double m_server_world_time_seconds = 0.0;
    float m_server_time_update_frequency = 0.5f;
    float m_server_time_update_accumulator = 0.0f;
};
}
