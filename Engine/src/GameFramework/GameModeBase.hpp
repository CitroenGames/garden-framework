#pragma once

#include "EngineExport.h"
#include "Components/Components.hpp"
#include "GameFramework/PlayerState.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <entt/entt.hpp>

class InputManager;
class PlayerController;
class world;

namespace GameFramework
{
class GameStateBase;

struct ENGINE_API PlayerLoginOptions
{
    uint16_t player_id = 0;
    std::string player_name;
    std::string options;
    std::string address;
    std::string portal;
    bool spectator = false;
};

struct ENGINE_API PlayerControllerEntry
{
    PlayerControllerEntry();
    ~PlayerControllerEntry();
    PlayerControllerEntry(PlayerControllerEntry&&) noexcept;
    PlayerControllerEntry& operator=(PlayerControllerEntry&&) noexcept;
    PlayerControllerEntry(const PlayerControllerEntry&) = delete;
    PlayerControllerEntry& operator=(const PlayerControllerEntry&) = delete;

    uint16_t player_id = 0;
    entt::entity controller_entity = entt::null;
    entt::entity player_state_entity = entt::null;
    std::unique_ptr<PlayerController> controller;
    PlayerStatePtr player_state;
    entt::entity pawn = entt::null;
    entt::entity freecam = entt::null;
    entt::entity start_spot = entt::null;
    std::string portal;
    bool spectator = false;
};

class ENGINE_API GameModeBase
{
public:
    GameModeBase();
    virtual ~GameModeBase();

    GameModeBase(const GameModeBase&) = delete;
    GameModeBase& operator=(const GameModeBase&) = delete;

    virtual const char* getClassName() const { return "GameModeBase"; }
    virtual const char* getDefaultGameStateClassName() const { return "GameStateBase"; }

    virtual void initGame(world& game_world,
                          const std::string& map_name,
                          const std::string& options,
                          std::string& error_message);
    virtual std::unique_ptr<GameStateBase> createGameState();
    virtual void initGameState();
    virtual void startPlay();
    virtual void tick(float delta_time);
    virtual void reset();

    world* getWorld() const { return m_world; }
    void bindWorld(world& game_world);
    GameStateBase* getGameState() const { return m_game_state; }

    void setInputManager(std::shared_ptr<InputManager> input_manager);
    std::shared_ptr<InputManager> getInputManager() const { return m_input_manager; }

    const std::string& getOptionsString() const { return m_options_string; }
    const std::string& getMapName() const { return m_map_name; }

    virtual bool hasMatchStarted() const;
    virtual bool hasMatchEnded() const;

    virtual bool setPause(uint16_t player_id = 0);
    virtual bool clearPause();
    virtual bool allowPausing(uint16_t player_id = 0) const;
    bool isPaused() const { return m_paused; }
    void setPauseable(bool pauseable) { m_pauseable = pauseable; syncGameModeComponent(); }
    bool isPauseable() const { return m_pauseable; }

    void setStartPlayersAsSpectators(bool enabled) { m_start_players_as_spectators = enabled; syncGameModeComponent(); }
    bool shouldStartPlayersAsSpectators() const { return m_start_players_as_spectators; }

    virtual bool preLogin(const PlayerLoginOptions& options, std::string& error_message);
    virtual PlayerControllerEntry* login(const PlayerLoginOptions& options, std::string& error_message);
    virtual void postLogin(PlayerControllerEntry& new_player);
    virtual void logout(uint16_t player_id);
    virtual void handleStartingNewPlayer(PlayerControllerEntry& new_player);
    virtual bool mustSpectate(const PlayerControllerEntry& player) const;
    virtual bool canSpectate(const PlayerControllerEntry& viewer, const PlayerState& view_target) const;

    PlayerControllerEntry* createLocalPlayer(const PlayerLoginOptions& options, std::string& error_message);
    virtual void changeName(PlayerControllerEntry& player, const std::string& new_name, bool name_change);
    void setDefaultPlayerName(std::string default_player_name);
    const std::string& getDefaultPlayerName() const { return m_default_player_name; }
    PlayerControllerEntry* getPrimaryPlayer();
    const PlayerControllerEntry* getPrimaryPlayer() const;
    PlayerController* getPrimaryPlayerController();
    const PlayerController* getPrimaryPlayerController() const;
    PlayerControllerEntry* getPlayer(uint16_t player_id);
    const PlayerControllerEntry* getPlayer(uint16_t player_id) const;
    const std::vector<PlayerControllerEntry>& getPlayers() const { return m_players; }

    virtual entt::entity choosePlayerStart(const PlayerControllerEntry& player);
    virtual entt::entity findPlayerStart(const PlayerControllerEntry& player, const std::string& incoming_name = "");
    virtual bool shouldSpawnAtStartSpot(const PlayerControllerEntry& player) const;
    virtual bool updatePlayerStartSpot(PlayerControllerEntry& player,
                                       const std::string& portal,
                                       std::string& out_error_message);
    virtual bool playerCanRestart(const PlayerControllerEntry& player) const;
    virtual void restartPlayer(PlayerControllerEntry& player);
    virtual void restartPlayerAtPlayerStart(PlayerControllerEntry& player, entt::entity start_spot);
    virtual void restartPlayerAtTransform(PlayerControllerEntry& player, const TransformComponent& spawn_transform);
    virtual entt::entity spawnDefaultPawnFor(PlayerControllerEntry& player, entt::entity start_spot);
    virtual entt::entity spawnDefaultPawnAtTransform(PlayerControllerEntry& player, const TransformComponent& spawn_transform);
    virtual void initStartSpot(entt::entity start_spot, PlayerControllerEntry& player);
    virtual void finishRestartPlayer(PlayerControllerEntry& player);
    virtual void failedToRestartPlayer(PlayerControllerEntry& player);
    virtual void setPlayerDefaults(entt::entity player_pawn);

    virtual float getPlayerRespawnDelay(uint16_t player_id) const;

    int32_t getNumPlayers() const;
    int32_t getNumSpectators() const;

protected:
    virtual void onPostLogin(PlayerControllerEntry& new_player);
    virtual void onLogout(PlayerControllerEntry& exiting_player);
    virtual void onChangeName(PlayerControllerEntry& player, const std::string& new_name, bool name_change);
    virtual void onRestartPlayer(PlayerControllerEntry& player);
    entt::entity findExistingPawnFor(const PlayerControllerEntry& player) const;
    entt::entity findOrCreateFreecamFor(const PlayerControllerEntry& player);
    uint16_t allocatePlayerId(uint16_t requested_id);
    void attachPlayerStateToGameState(const PlayerStatePtr& player_state);
    void detachPlayerStateFromGameState(uint16_t player_id);
    virtual void syncGameModeComponent();
    void syncPlayerEntryToEcs(PlayerControllerEntry& player);
    void destroyPlayerEntryEcs(PlayerControllerEntry& player);

    world* m_world = nullptr;
    GameStateBase* m_game_state = nullptr;
    std::shared_ptr<InputManager> m_input_manager;
    std::string m_map_name;
    std::string m_options_string;
    std::string m_default_player_name = "Player";
    std::vector<PlayerControllerEntry> m_players;
    bool m_pauseable = true;
    bool m_paused = false;
    bool m_start_players_as_spectators = false;
    uint16_t m_next_player_id = 1;
};
}
