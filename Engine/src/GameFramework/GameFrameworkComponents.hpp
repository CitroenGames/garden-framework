#pragma once

#include "EngineExport.h"
#include "GameFramework/MatchState.hpp"
#include "GameFramework/PlayerState.hpp"
#include "Components/IdentityComponent.hpp"
#include "Reflection/Reflector.hpp"

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

    static void reflect(Reflector<GameModeComponent>& r)
    {
        r.display("Game Mode").category("GameFramework").removable(false);
        r.field<&GameModeComponent::class_name>("class_name").visible().category("Class");
        r.field<&GameModeComponent::map_name>("map_name").category("Match");
        r.field<&GameModeComponent::options>("options").category("Match");
        r.field<&GameModeComponent::default_player_name>("default_player_name").category("Match");
        r.field<&GameModeComponent::authority>("authority").visible().category("State");
        r.field<&GameModeComponent::pauseable>("pauseable").category("Match");
        r.field<&GameModeComponent::paused>("paused").visible().category("State");
        r.field<&GameModeComponent::start_players_as_spectators>("start_players_as_spectators").category("Match");
        r.field<&GameModeComponent::delayed_start>("delayed_start").category("Match");
        r.field<&GameModeComponent::match_state>("match_state").visible().category("State");
        r.field<&GameModeComponent::min_respawn_delay>("min_respawn_delay").drag(0.01f).range(0.0f, 60.0f).category("Match");
        r.field<&GameModeComponent::num_players>("num_players").visible().category("State");
        r.field<&GameModeComponent::num_spectators>("num_spectators").visible().category("State");
        r.field<&GameModeComponent::num_bots>("num_bots").visible().category("State");
        r.field<&GameModeComponent::num_travelling_players>("num_travelling_players").visible().category("State");
    }
};

struct ENGINE_API GameStateComponent
{
    std::string class_name = "GameState";
    std::string game_mode_class_name = "GameMode";
    std::string spectator_class_name;
    std::string match_state = MatchState::EnteringMap;
    std::string previous_match_state = MatchState::EnteringMap;
    std::vector<EntityRef> player_states;
    bool has_begun_play = false;
    bool match_started = false;
    bool match_ended = false;
    double server_world_time_seconds = 0.0;
    float server_time_update_frequency = 0.1f;
    int32_t elapsed_time = 0;
    int32_t num_players = 0;
    int32_t num_spectators = 0;

    static void reflect(Reflector<GameStateComponent>& r)
    {
        r.display("Game State").category("GameFramework").removable(false);
        r.field<&GameStateComponent::class_name>("class_name").visible().category("Class");
        r.field<&GameStateComponent::game_mode_class_name>("game_mode_class_name").visible().category("Class");
        r.field<&GameStateComponent::spectator_class_name>("spectator_class_name").visible().category("Class");
        r.field<&GameStateComponent::match_state>("match_state").visible().category("State");
        r.field<&GameStateComponent::previous_match_state>("previous_match_state").visible().category("State");
        r.field<&GameStateComponent::player_states>("player_states").visible().category("Players");
        r.field<&GameStateComponent::has_begun_play>("has_begun_play").visible().category("State");
        r.field<&GameStateComponent::match_started>("match_started").visible().category("State");
        r.field<&GameStateComponent::match_ended>("match_ended").visible().category("State");
        r.field<&GameStateComponent::server_world_time_seconds>("server_world_time_seconds").visible().category("State");
        r.field<&GameStateComponent::server_time_update_frequency>("server_time_update_frequency").visible().category("State");
        r.field<&GameStateComponent::elapsed_time>("elapsed_time").visible().category("State");
        r.field<&GameStateComponent::num_players>("num_players").visible().category("State");
        r.field<&GameStateComponent::num_spectators>("num_spectators").visible().category("State");
    }
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
    EntityRef pawn;
    EntityRef freecam;

    static void reflect(Reflector<PlayerStateComponent>& r)
    {
        r.display("Player State").category("GameFramework");
        r.field<&PlayerStateComponent::player_id>("player_id").visible().category("Player");
        r.field<&PlayerStateComponent::player_name>("player_name").category("Player");
        r.field<&PlayerStateComponent::score>("score").drag(0.1f).category("Player");
        r.field<&PlayerStateComponent::ping_ms>("ping_ms").visible().category("Player");
        r.field<&PlayerStateComponent::is_spectator>("is_spectator").category("Player");
        r.field<&PlayerStateComponent::is_inactive>("is_inactive").visible().category("Player");
        r.field<&PlayerStateComponent::start_time>("start_time").visible().category("Player");
        r.field<&PlayerStateComponent::pawn>("pawn").widget(EPropertyWidget::EntityRef).category("Refs");
        r.field<&PlayerStateComponent::freecam>("freecam").widget(EPropertyWidget::EntityRef).category("Refs");
    }
};

struct ENGINE_API PlayerControllerComponent
{
    uint16_t player_id = 0;
    EntityRef player_state;
    EntityRef pawn;
    EntityRef freecam;
    EntityRef start_spot;
    std::string portal;
    bool spectator = false;
    bool local = false;

    static void reflect(Reflector<PlayerControllerComponent>& r)
    {
        r.display("Player Controller").category("GameFramework");
        r.field<&PlayerControllerComponent::player_id>("player_id").visible().category("Player");
        r.field<&PlayerControllerComponent::player_state>("player_state").widget(EPropertyWidget::EntityRef).category("Refs");
        r.field<&PlayerControllerComponent::pawn>("pawn").widget(EPropertyWidget::EntityRef).category("Refs");
        r.field<&PlayerControllerComponent::freecam>("freecam").widget(EPropertyWidget::EntityRef).category("Refs");
        r.field<&PlayerControllerComponent::start_spot>("start_spot").widget(EPropertyWidget::EntityRef).category("Refs");
        r.field<&PlayerControllerComponent::portal>("portal").category("Player");
        r.field<&PlayerControllerComponent::spectator>("spectator").category("Player");
        r.field<&PlayerControllerComponent::local>("local").visible().category("Player");
    }
};

ENGINE_API entt::entity getGameModeEntity(const entt::registry& registry);
ENGINE_API entt::entity getOrCreateGameModeEntity(entt::registry& registry);
ENGINE_API entt::entity getGameStateEntity(const entt::registry& registry);
ENGINE_API entt::entity getOrCreateGameStateEntity(entt::registry& registry);
ENGINE_API entt::entity findPlayerStateEntity(const entt::registry& registry, uint16_t player_id);
ENGINE_API PlayerStateComponent* getPlayerStateComponent(entt::registry& registry, uint16_t player_id);
ENGINE_API const PlayerStateComponent* getPlayerStateComponent(const entt::registry& registry, uint16_t player_id);
ENGINE_API void copyPlayerStateToComponent(entt::registry& registry, PlayerStateComponent& component, const PlayerState& player_state);
}
