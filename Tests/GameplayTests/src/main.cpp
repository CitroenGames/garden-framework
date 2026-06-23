#include "Components/Components.hpp"
#include "GameFramework/GameFrameworkComponents.hpp"
#include "GameFramework/GameMode.hpp"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameState.hpp"
#include "LevelManager.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "world.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace
{
bool approx(float a, float b, float epsilon = 0.001f)
{
    return std::abs(a - b) <= epsilon;
}

bool approxVec3(const glm::vec3& a, const glm::vec3& b, float epsilon = 0.001f)
{
    return glm::distance(a, b) <= epsilon;
}

bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

class TestDefaultGameMode : public GameFramework::GameMode
{
public:
    const char* getClassName() const override { return "TestDefaultGameMode"; }
};

class TestDefaultGameState : public GameFramework::GameState
{
public:
    const char* getClassName() const override { return "TestDefaultGameState"; }
};

class HookedGameMode : public GameFramework::GameMode
{
public:
    const char* getClassName() const override { return "HookedGameMode"; }

    entt::entity spawnDefaultPawnFor(GameFramework::PlayerControllerEntry& player,
                                     entt::entity start_spot) override
    {
        ++spawn_default_pawn_for_count;
        return GameFramework::GameMode::spawnDefaultPawnFor(player, start_spot);
    }

    int post_login_count = 0;
    int logout_count = 0;
    int change_name_count = 0;
    int restart_player_count = 0;
    int spawn_default_pawn_for_count = 0;
    uint16_t logout_player_id = 0;
    bool restarted_after_post_login = false;
    std::vector<std::string> set_match_state_events;

protected:
    void onPostLogin(GameFramework::PlayerControllerEntry& new_player) override
    {
        (void)new_player;
        ++post_login_count;
    }

    void onLogout(GameFramework::PlayerControllerEntry& exiting_player) override
    {
        logout_player_id = exiting_player.player_id;
        ++logout_count;
    }

    void onChangeName(GameFramework::PlayerControllerEntry& player,
                      const std::string& new_name,
                      bool name_change) override
    {
        (void)player;
        (void)new_name;
        (void)name_change;
        ++change_name_count;
    }

    void onRestartPlayer(GameFramework::PlayerControllerEntry& player) override
    {
        (void)player;
        restarted_after_post_login = post_login_count > 0;
        ++restart_player_count;
    }

    void onSetMatchState(const std::string& new_state) override
    {
        set_match_state_events.push_back(new_state);
    }
};

class RecordingGameState : public GameFramework::GameState
{
public:
    const char* getClassName() const override { return "RecordingGameState"; }

    std::string previous_seen_while_waiting;
    std::string previous_seen_while_starting;

protected:
    void handleMatchIsWaitingToStart() override
    {
        previous_seen_while_waiting = getPreviousMatchState();
        GameFramework::GameState::handleMatchIsWaitingToStart();
    }

    void handleMatchHasStarted() override
    {
        previous_seen_while_starting = getPreviousMatchState();
        GameFramework::GameState::handleMatchHasStarted();
    }
};

bool testPlayerStartReflectionRegistered()
{
    const std::string name = "player start component is reflected";

    ReflectionRegistry registry;
    registerEngineReflection(registry);

    const ComponentDescriptor* desc = registry.findByName("PlayerStartComponent");
    if (!desc)
        return fail(name, "PlayerStartComponent was not registered");
    if (desc->display_name != "Player Start")
        return fail(name, "unexpected display name");

    return pass(name);
}

bool testGameModeUsesPortalPlayerStart()
{
    const std::string name = "game mode uses portal player start";

    world game_world;

    entt::entity start_a = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(start_a, "StartA");
    game_world.registry.emplace<TransformComponent>(start_a, -5.0f, 0.0f, 0.0f);
    game_world.registry.emplace<PlayerStartComponent>(start_a).tag = "A";

    entt::entity start_b = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(start_b, "StartB");
    game_world.registry.emplace<TransformComponent>(start_b, 42.0f, 3.0f, 7.0f);
    game_world.registry.emplace<PlayerStartComponent>(start_b).tag = "B";

    auto mode = std::make_unique<GameFramework::GameMode>();
    GameFramework::GameMode* mode_ptr = mode.get();
    game_world.setAuthorityGameMode(std::move(mode));

    std::string error;
    mode_ptr->initGame(game_world, "PortalMap", "", error);
    if (!error.empty())
        return fail(name, "initGame returned error: " + error);

    GameFramework::PlayerLoginOptions login;
    login.player_id = 9;
    login.player_name = "PortalUser";
    login.portal = "B";
    GameFramework::PlayerControllerEntry* player = mode_ptr->createLocalPlayer(login, error);
    if (!player || !error.empty())
        return fail(name, "local player login failed: " + error);
    if (player->start_spot != start_b)
        return fail(name, "portal did not assign the matching PlayerStart");

    mode_ptr->startPlay();

    if (!game_world.registry.valid(player->pawn))
        return fail(name, "portal player did not spawn");

    const auto& transform = game_world.registry.get<TransformComponent>(player->pawn);
    if (!approxVec3(transform.position, glm::vec3(42.0f, 3.0f, 7.0f)))
        return fail(name, "portal player did not spawn at tagged PlayerStart");

    const auto* controller_component =
        game_world.registry.try_get<GameFramework::PlayerControllerComponent>(player->controller_entity);
    if (!controller_component ||
        controller_component->start_spot != start_b ||
        controller_component->portal != "B")
    {
        return fail(name, "portal start spot was not mirrored to the PlayerController component");
    }

    return pass(name);
}

bool testGameModeUnrealStyleHooks()
{
    const std::string name = "game mode unreal style hooks";

    world game_world;

    entt::entity start = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(start, "HookStart");
    game_world.registry.emplace<TransformComponent>(start, 1.0f, 2.0f, 3.0f);
    game_world.registry.emplace<PlayerStartComponent>(start);

    auto mode = std::make_unique<HookedGameMode>();
    HookedGameMode* mode_ptr = mode.get();
    game_world.setAuthorityGameMode(std::move(mode));

    std::string error;
    mode_ptr->initGame(game_world, "HookMap", "", error);
    if (!error.empty())
        return fail(name, "initGame returned error: " + error);

    mode_ptr->setDefaultPlayerName("Hero");

    GameFramework::PlayerLoginOptions login;
    login.player_id = 5;
    GameFramework::PlayerControllerEntry* player = mode_ptr->createLocalPlayer(login, error);
    if (!player || !error.empty())
        return fail(name, "local player login failed: " + error);

    if (!player->player_state || player->player_state->player_name != "Hero 5")
        return fail(name, "default player name was not applied through ChangeName");
    if (mode_ptr->change_name_count != 1)
        return fail(name, "onChangeName was not called");
    if (mode_ptr->post_login_count != 1)
        return fail(name, "onPostLogin was not called");
    if (mode_ptr->restart_player_count != 0)
        return fail(name, "player restarted before match start");

    mode_ptr->startPlay();

    if (mode_ptr->spawn_default_pawn_for_count != 1)
        return fail(name, "RestartPlayerAtPlayerStart did not use spawnDefaultPawnFor");
    if (mode_ptr->restart_player_count != 1 || !mode_ptr->restarted_after_post_login)
        return fail(name, "onRestartPlayer was not called after onPostLogin");
    if (mode_ptr->set_match_state_events.size() != 2 ||
        mode_ptr->set_match_state_events[0] != GameFramework::MatchState::WaitingToStart ||
        mode_ptr->set_match_state_events[1] != GameFramework::MatchState::InProgress)
    {
        return fail(name, "onSetMatchState did not run after match state transitions");
    }

    entt::entity mode_entity = GameFramework::getGameModeEntity(game_world.registry);
    const auto& mode_component =
        game_world.registry.get<GameFramework::GameModeComponent>(mode_entity);
    if (mode_component.default_player_name != "Hero")
        return fail(name, "default player name was not mirrored to GameMode component");

    const uint16_t player_id = player->player_id;
    mode_ptr->logout(player_id);
    if (mode_ptr->logout_count != 1 || mode_ptr->logout_player_id != player_id)
        return fail(name, "onLogout was not called before player cleanup");
    if (!mode_ptr->getPlayers().empty())
        return fail(name, "player was not removed after logout");

    return pass(name);
}

bool testGameModeSpawnsAtPlayerStart()
{
    const std::string name = "game mode spawns player at player start";

    world game_world;

    entt::entity start = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(start, "StartA");
    game_world.registry.emplace<TransformComponent>(start, 10.0f, 2.0f, -3.0f);
    auto& start_component = game_world.registry.emplace<PlayerStartComponent>(start);
    start_component.tag = "A";

    entt::entity pawn = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(pawn, "ExistingPlayer");
    game_world.registry.emplace<TransformComponent>(pawn, 0.0f, 0.0f, 0.0f);
    game_world.registry.emplace<PlayerComponent>(pawn);
    game_world.registry.emplace<RigidBodyComponent>(pawn);

    auto mode = std::make_unique<GameFramework::GameMode>();
    GameFramework::GameMode* mode_ptr = mode.get();
    game_world.setAuthorityGameMode(std::move(mode));

    std::string error;
    mode_ptr->initGame(game_world, "TestMap", "", error);
    if (!error.empty())
        return fail(name, "initGame returned error: " + error);

    GameFramework::PlayerLoginOptions login;
    login.player_id = 1;
    login.player_name = "Tester";
    GameFramework::PlayerControllerEntry* player = mode_ptr->createLocalPlayer(login, error);
    if (!player || !error.empty())
        return fail(name, "local player login failed: " + error);
    if (mode_ptr->hasMatchStarted())
        return fail(name, "match started before StartPlay");
    if (player->pawn != entt::null)
        return fail(name, "player pawn spawned before match start");

    mode_ptr->startPlay();

    if (player->pawn != pawn)
        return fail(name, "GameMode did not possess the existing player pawn");

    const auto& transform = game_world.registry.get<TransformComponent>(pawn);
    if (!approxVec3(transform.position, glm::vec3(10.0f, 2.0f, -3.0f)))
        return fail(name, "pawn was not moved to PlayerStart transform");

    if (mode_ptr->getNumPlayers() != 1)
        return fail(name, "NumPlayers was not updated");
    if (!mode_ptr->hasMatchStarted())
        return fail(name, "match did not start after player login");

    auto* game_state = game_world.getGameStateAs<GameFramework::GameState>();
    if (!game_state)
        return fail(name, "GameState was not created");
    if (game_state->getPlayerArray().size() != 1)
        return fail(name, "GameState did not receive PlayerState");
    if (game_state->getMatchState() != GameFramework::MatchState::InProgress)
        return fail(name, "GameState match state was not synchronized");

    entt::entity mode_entity = GameFramework::getGameModeEntity(game_world.registry);
    if (!game_world.registry.valid(mode_entity))
        return fail(name, "GameMode ECS entity was not created");
    const auto& mode_component =
        game_world.registry.get<GameFramework::GameModeComponent>(mode_entity);
    if (mode_component.class_name != "GameMode" ||
        mode_component.match_state != GameFramework::MatchState::InProgress ||
        mode_component.num_players != 1)
        return fail(name, "GameMode ECS component was not synchronized");

    entt::entity state_entity = GameFramework::getGameStateEntity(game_world.registry);
    if (!game_world.registry.valid(state_entity))
        return fail(name, "GameState ECS entity was not created");
    const auto& state_component =
        game_world.registry.get<GameFramework::GameStateComponent>(state_entity);
    if (state_component.match_state != GameFramework::MatchState::InProgress ||
        state_component.player_states.size() != 1 ||
        state_component.num_players != 1 ||
        !state_component.has_begun_play)
        return fail(name, "GameState ECS component was not synchronized");

    const auto* player_state_component =
        GameFramework::getPlayerStateComponent(game_world.registry, 1);
    if (!player_state_component)
        return fail(name, "PlayerState ECS component was not created");
    if (player_state_component->player_name != "Tester" ||
        player_state_component->pawn != pawn)
        return fail(name, "PlayerState ECS component was not synchronized");

    int controller_count = 0;
    auto controller_view = game_world.registry.view<GameFramework::PlayerControllerComponent>();
    for (entt::entity controller_entity : controller_view)
    {
        const auto& controller_component =
            controller_view.get<GameFramework::PlayerControllerComponent>(controller_entity);
        if (controller_component.player_id == 1 &&
            controller_component.player_state == state_component.player_states.front() &&
            controller_component.pawn == pawn)
            ++controller_count;
    }
    if (controller_count != 1)
        return fail(name, "PlayerController ECS component was not synchronized");

    return pass(name);
}

bool testDelayedStartWaitsForManualMatchStart()
{
    const std::string name = "delayed start waits for manual match start";

    world game_world;

    auto mode = std::make_unique<GameFramework::GameMode>();
    GameFramework::GameMode* mode_ptr = mode.get();
    game_world.setAuthorityGameMode(std::move(mode));

    std::string error;
    mode_ptr->initGame(game_world, "DelayedMap", "", error);
    if (!error.empty())
        return fail(name, "initGame returned error: " + error);

    mode_ptr->setDelayedStart(true);

    GameFramework::PlayerLoginOptions login;
    login.player_id = 7;
    login.player_name = "Delayed";
    GameFramework::PlayerControllerEntry* player = mode_ptr->createLocalPlayer(login, error);
    if (!player || !error.empty())
        return fail(name, "local player login failed: " + error);

    mode_ptr->startPlay();
    if (mode_ptr->hasMatchStarted())
        return fail(name, "delayed match started automatically");
    if (mode_ptr->getMatchState() != GameFramework::MatchState::WaitingToStart)
        return fail(name, "delayed match did not enter WaitingToStart");
    if (player->pawn != entt::null)
        return fail(name, "delayed player pawn spawned before manual start");

    auto* game_state = game_world.getGameStateAs<GameFramework::GameState>();
    if (!game_state)
        return fail(name, "GameState was not created");
    if (!game_state->hasBegunPlay())
        return fail(name, "GameState did not begin play while waiting to start");
    if (game_state->hasMatchStarted())
        return fail(name, "GameState reported match started before manual match start");

    mode_ptr->startMatch();
    if (!mode_ptr->isMatchInProgress())
        return fail(name, "manual start did not put match in progress");
    if (!game_world.registry.valid(player->pawn))
        return fail(name, "manual start did not spawn a player pawn");
    if (!game_state->hasMatchStarted())
        return fail(name, "GameState did not report match started after manual match start");

    return pass(name);
}

bool testGameStateMatchStatePreviousBookkeeping()
{
    const std::string name = "game state match state previous bookkeeping";

    world game_world;
    auto state = std::make_unique<RecordingGameState>();
    RecordingGameState* state_ptr = state.get();
    game_world.setGameState(std::move(state));
    state_ptr->initialize(&game_world);

    state_ptr->setMatchState(GameFramework::MatchState::WaitingToStart);
    if (state_ptr->previous_seen_while_waiting != GameFramework::MatchState::EnteringMap)
        return fail(name, "WaitingToStart callback did not see EnteringMap as previous state");
    if (state_ptr->getPreviousMatchState() != GameFramework::MatchState::WaitingToStart)
        return fail(name, "PreviousMatchState was not advanced after WaitingToStart notification");

    state_ptr->setMatchState(GameFramework::MatchState::InProgress);
    if (state_ptr->previous_seen_while_starting != GameFramework::MatchState::WaitingToStart)
        return fail(name, "InProgress callback did not see WaitingToStart as previous state");
    if (state_ptr->getPreviousMatchState() != GameFramework::MatchState::InProgress)
        return fail(name, "PreviousMatchState was not advanced after InProgress notification");

    return pass(name);
}

bool testLevelMetadataAppliesGameplaySettings()
{
    const std::string name = "level metadata applies gameplay settings";

    LevelData data;
    data.metadata.game_mode_class = "GameMode";
    data.metadata.game_state_class = "GameState";
    data.metadata.delayed_start = true;
    data.metadata.start_players_as_spectators = true;
    data.metadata.pauseable = false;
    data.metadata.min_respawn_delay = 2.5f;

    world game_world;
    LevelManager level_manager;
    if (!level_manager.instantiateLevelParallel(data, game_world, nullptr))
        return fail(name, "empty level instantiation failed");

    auto* mode = game_world.getAuthorityGameModeAs<GameFramework::GameMode>();
    if (!mode)
        return fail(name, "GameMode was not created from metadata");
    if (!mode->isDelayedStart())
        return fail(name, "delayed start was not applied");
    if (!mode->shouldStartPlayersAsSpectators())
        return fail(name, "start-as-spectator flag was not applied");
    if (mode->isPauseable())
        return fail(name, "pauseable flag was not applied");
    if (!approx(mode->getMinRespawnDelay(), 2.5f))
        return fail(name, "min respawn delay was not applied");

    if (!game_world.getGameStateAs<GameFramework::GameState>())
        return fail(name, "GameState was not created from metadata");

    entt::entity mode_entity = GameFramework::getGameModeEntity(game_world.registry);
    if (!game_world.registry.valid(mode_entity))
        return fail(name, "metadata did not create GameMode ECS entity");
    const auto& mode_component =
        game_world.registry.get<GameFramework::GameModeComponent>(mode_entity);
    if (!mode_component.delayed_start ||
        !mode_component.start_players_as_spectators ||
        mode_component.pauseable ||
        !approx(mode_component.min_respawn_delay, 2.5f))
        return fail(name, "metadata was not mirrored to GameMode ECS component");

    return pass(name);
}

bool testProjectDefaultsResolveGameplayClasses()
{
    const std::string name = "project defaults resolve gameplay classes";
    const std::string source_id = "gameplay_tests";

    auto& registry = GameFramework::GameModeRegistry::get();
    registry.unregisterBySource(source_id);
    registry.registerGameMode(
        "TestDefaultGameMode",
        []() { return std::make_unique<TestDefaultGameMode>(); },
        source_id);
    registry.registerGameState(
        "TestDefaultGameState",
        []() { return std::make_unique<TestDefaultGameState>(); },
        source_id);

    LevelData data;
    data.metadata.game_mode_class.clear();
    data.metadata.game_state_class.clear();

    world game_world;
    LevelManager level_manager;
    level_manager.setGameplayDefaults("TestDefaultGameMode", "TestDefaultGameState");

    const bool instantiated = level_manager.instantiateLevelParallel(data, game_world, nullptr);
    registry.unregisterBySource(source_id);

    if (!instantiated)
        return fail(name, "empty level instantiation failed");

    auto* mode = game_world.getAuthorityGameMode();
    if (!mode || std::string(mode->getClassName()) != "TestDefaultGameMode")
        return fail(name, "project default GameMode was not created");

    auto* state = game_world.getGameState();
    if (!state || std::string(state->getClassName()) != "TestDefaultGameState")
        return fail(name, "project default GameState was not created");

    return pass(name);
}

bool testClientWorldCreatesOnlyGameState()
{
    const std::string name = "client world creates only game state";

    LevelData data;
    data.metadata.game_mode_class = "GameMode";
    data.metadata.game_state_class = "GameState";

    world game_world;
    LevelManager level_manager;
    if (!level_manager.instantiateLevelParallel(
            data, game_world, nullptr, nullptr, nullptr, nullptr, false))
    {
        return fail(name, "empty client level instantiation failed");
    }

    if (game_world.getAuthorityGameMode())
        return fail(name, "client world created an authority GameMode");

    auto* game_state = game_world.getGameStateAs<GameFramework::GameState>();
    if (!game_state)
        return fail(name, "client world did not create GameState");
    if (game_state->getAuthorityGameMode())
        return fail(name, "client GameState has authority GameMode");
    if (game_state->getGameModeClassName() != "GameMode")
        return fail(name, "client GameState did not mirror GameMode class");

    if (!game_world.initializeGameplayFramework("ClientMap", ""))
        return fail(name, "client GameState did not initialize");

    game_world.startGameplayFramework();
    if (!game_state->hasBegunPlay())
        return fail(name, "client GameState did not begin play");

    game_world.tickGameplayFramework(0.25f);
    if (game_state->getServerWorldTimeSeconds() <= 0.0)
        return fail(name, "client GameState did not tick");

    return pass(name);
}

bool testGameModeRegistryCreatesBuiltins()
{
    const std::string name = "game mode registry creates builtins";

    auto mode = GameFramework::GameModeRegistry::get().createGameMode("GameMode");
    if (!mode || std::string(mode->getClassName()) != "GameMode")
        return fail(name, "failed to create built-in GameMode");

    auto state = GameFramework::GameModeRegistry::get().createGameState("GameState");
    if (!state || std::string(state->getClassName()) != "GameState")
        return fail(name, "failed to create built-in GameState");

    return pass(name);
}
}

int main()
{
    bool ok = true;
    ok = testPlayerStartReflectionRegistered() && ok;
    ok = testGameModeRegistryCreatesBuiltins() && ok;
    ok = testGameModeUsesPortalPlayerStart() && ok;
    ok = testGameModeUnrealStyleHooks() && ok;
    ok = testGameModeSpawnsAtPlayerStart() && ok;
    ok = testDelayedStartWaitsForManualMatchStart() && ok;
    ok = testGameStateMatchStatePreviousBookkeeping() && ok;
    ok = testLevelMetadataAppliesGameplaySettings() && ok;
    ok = testProjectDefaultsResolveGameplayClasses() && ok;
    ok = testClientWorldCreatesOnlyGameState() && ok;
    return ok ? 0 : 1;
}
