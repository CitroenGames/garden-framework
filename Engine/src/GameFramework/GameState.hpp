#pragma once

#include "GameFramework/GameStateBase.hpp"
#include "GameFramework/MatchState.hpp"

#include <string>

namespace GameFramework
{
class ENGINE_API GameState : public GameStateBase
{
public:
    GameState();
    ~GameState() override;

    const char* getClassName() const override { return "GameState"; }

    void tick(float delta_time) override;
    void handleBeginPlay() override;
    bool hasMatchStarted() const override;
    bool hasMatchEnded() const override;
    float getPlayerStartTime(uint16_t player_id) const override;
    float getPlayerRespawnDelay(uint16_t player_id) const override;

    const std::string& getMatchState() const { return m_match_state; }
    const std::string& getPreviousMatchState() const { return m_previous_match_state; }
    bool isMatchInProgress() const;

    virtual void setMatchState(const std::string& new_state);
    int32_t getElapsedTime() const { return m_elapsed_time; }
    virtual void defaultTimer();

protected:
    virtual void onMatchStateSet();
    virtual void handleMatchIsWaitingToStart();
    virtual void handleMatchHasStarted();
    virtual void handleMatchHasEnded();
    virtual void handleLeavingMap();
    virtual void handleMatchAborted();

    std::string m_match_state = MatchState::EnteringMap;
    std::string m_previous_match_state = MatchState::EnteringMap;
    int32_t m_elapsed_time = 0;
    float m_elapsed_accumulator = 0.0f;
};
}
