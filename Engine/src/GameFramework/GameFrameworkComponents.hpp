#pragma once

#include "EngineExport.h"
#include "GameFramework/MatchState.hpp"
#include "GameFramework/PlayerState.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <entt/entt.hpp>

namespace GameFramework
{
struct ENGINE_API GameModeComponent
{
    std::string class_name = "GameMode";
    std::string map_name;
    std::string options;
    std::string default_player_name = "Player";
    bool authority = true;
    bool pauseable = true;
    bool paused = false;
    bool start_players_as_spectators = false;
    bool delayed_start = false;
    std::string match_state = MatchState::EnteringMap;
    float min_respawn_delay = 0.0f;
    int32_t num_players = 0;
    int32_t num_spectators = 0;
    int32_t num_bots = 0;
    int32_t num_travelling_players = 0;
};

struct ENGINE_API GameStateComponent
{
    std::string class_name = "GameState";
    std::string game_mode_class_name = "GameMode";
    std::string spectator_class_name;
    std::string match_state = MatchState::EnteringMap;
    std::string previous_match_state = MatchState::EnteringMap;
    std::vector<entt::entity> player_states;
    bool has_begun_play = false;
    bool match_started = false;
    bool match_ended = false;
    double server_world_time_seconds = 0.0;
    float server_time_update_frequency = 0.1f;
    int32_t elapsed_time = 0;
    int32_t num_players = 0;
    int32_t num_spectators = 0;
};

struct ENGINE_API PlayerStateComponent
{
    uint16_t player_id = 0;
    std::string player_name = "Player";
    float score = 0.0f;
    int32_t ping_ms = 0;
    bool is_spectator = false;
    bool is_inactive = false;
    float start_time = 0.0f;
    entt::entity pawn = entt::null;
    entt::entity freecam = entt::null;
};

struct ENGINE_API PlayerControllerComponent
{
    uint16_t player_id = 0;
    entt::entity player_state = entt::null;
    entt::entity pawn = entt::null;
    entt::entity freecam = entt::null;
    entt::entity start_spot = entt::null;
    std::string portal;
    bool spectator = false;
    bool local = false;
};

ENGINE_API entt::entity getGameModeEntity(const entt::registry& registry);
ENGINE_API entt::entity getOrCreateGameModeEntity(entt::registry& registry);
ENGINE_API entt::entity getGameStateEntity(const entt::registry& registry);
ENGINE_API entt::entity getOrCreateGameStateEntity(entt::registry& registry);
ENGINE_API entt::entity findPlayerStateEntity(const entt::registry& registry, uint16_t player_id);
ENGINE_API PlayerStateComponent* getPlayerStateComponent(entt::registry& registry, uint16_t player_id);
ENGINE_API const PlayerStateComponent* getPlayerStateComponent(const entt::registry& registry, uint16_t player_id);
ENGINE_API void copyPlayerStateToComponent(PlayerStateComponent& component, const PlayerState& player_state);
}
