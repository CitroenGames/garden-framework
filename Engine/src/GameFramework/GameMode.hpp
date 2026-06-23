#pragma once

#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/MatchState.hpp"

#include <memory>
#include <string>

namespace GameFramework
{
class ENGINE_API GameMode : public GameModeBase
{
public:
    GameMode();
    ~GameMode() override;

    const char* getClassName() const override { return "GameMode"; }
    const char* getDefaultGameStateClassName() const override { return "GameState"; }

    std::unique_ptr<GameStateBase> createGameState() override;
    void initGame(world& game_world,
                  const std::string& map_name,
                  const std::string& options,
                  std::string& error_message) override;
    void startPlay() override;
    void tick(float delta_time) override;
    bool hasMatchStarted() const override;
    bool hasMatchEnded() const override;
    void postLogin(PlayerControllerEntry& new_player) override;
    void logout(uint16_t player_id) override;
    void handleStartingNewPlayer(PlayerControllerEntry& new_player) override;
    bool playerCanRestart(const PlayerControllerEntry& player) const override;
    float getPlayerRespawnDelay(uint16_t player_id) const override;

    const std::string& getMatchState() const { return m_match_state; }
    bool isMatchInProgress() const;

    virtual void startMatch();
    virtual void endMatch();
    virtual void restartGame();
    virtual void abortMatch();
    virtual void startToLeaveMap();

    void setDelayedStart(bool delayed_start) { m_delayed_start = delayed_start; syncGameModeComponent(); }
    bool isDelayedStart() const { return m_delayed_start; }

    void setMinRespawnDelay(float delay);
    float getMinRespawnDelay() const { return m_min_respawn_delay; }

    int32_t getNumBots() const { return m_num_bots; }
    int32_t getNumTravellingPlayers() const { return m_num_travelling_players; }

protected:
    void syncGameModeComponent() override;
    virtual void setMatchState(const std::string& new_state);
    virtual void onMatchStateSet();
    virtual void onSetMatchState(const std::string& new_state);
    virtual void handleMatchIsWaitingToStart();
    virtual bool readyToStartMatch() const;
    virtual void handleMatchHasStarted();
    virtual bool readyToEndMatch() const;
    virtual void handleMatchHasEnded();
    virtual void handleLeavingMap();
    virtual void handleMatchAborted();

    std::string m_match_state = MatchState::EnteringMap;
    bool m_delayed_start = false;
    int32_t m_num_bots = 0;
    int32_t m_num_travelling_players = 0;
    float m_min_respawn_delay = 1.0f;
};
}
