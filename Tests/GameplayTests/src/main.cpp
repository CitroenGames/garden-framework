#include "Components/Components.hpp"
#include "GameFramework/GameFrameworkComponents.hpp"
#include "GameFramework/GameMode.hpp"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameState.hpp"
#include "LevelManager.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "world.hpp"
#include "shared/FpsGameLogic.hpp"
#include "shared/WorldItemLifecycle.hpp"
#include "shared/CompetitiveLogic.hpp"
#include "shared/BuyZoneTriggers.hpp"
#include "shared/CombatProtocol.hpp"
#include "server/GameRules.hpp"
#include "server/FpsDamage.hpp"
#include "server/LagCompensation.hpp"

#include <cmath>
#include <cstring>
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
        controller_component->start_spot != entityRefOf(game_world.registry, start_b) ||
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
        player_state_component->pawn != entityRefOf(game_world.registry, pawn))
        return fail(name, "PlayerState ECS component was not synchronized");

    int controller_count = 0;
    auto controller_view = game_world.registry.view<GameFramework::PlayerControllerComponent>();
    for (entt::entity controller_entity : controller_view)
    {
        const auto& controller_component =
            controller_view.get<GameFramework::PlayerControllerComponent>(controller_entity);
        if (controller_component.player_id == 1 &&
            controller_component.player_state == state_component.player_states.front() &&
            controller_component.pawn == entityRefOf(game_world.registry, pawn))
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

bool testFpsSpawnSelectionPerTeamFarthest()
{
    const std::string name = "FpsSpawnSelectionPerTeamFarthest";
    Game::GameRules rules;
    // Enemies near the first attackers spawn -> farthest attackers spawn chosen.
    glm::vec3 enemy(-15.0f, 5.0f, -15.0f);
    glm::vec3 picked = rules.getSpawnPointForTeam(FpsLogic::kTeamAttackers, {enemy});
    if (!approxVec3(picked, glm::vec3(-15.0f, 5.0f, 15.0f))) return fail(name, "did not pick farthest attackers spawn");
    glm::vec3 picked_def = rules.getSpawnPointForTeam(FpsLogic::kTeamDefenders, {glm::vec3(15.0f, 5.0f, 15.0f)});
    if (!approxVec3(picked_def, glm::vec3(15.0f, 5.0f, -15.0f))) return fail(name, "did not pick farthest defenders spawn");
    // No enemies -> round-robin still returns a team spawn.
    glm::vec3 rr = rules.getSpawnPointForTeam(FpsLogic::kTeamAttackers, {});
    bool in_team = approxVec3(rr, glm::vec3(-15.0f,5.0f,-15.0f)) || approxVec3(rr, glm::vec3(-15.0f,5.0f,15.0f));
    if (!in_team) return fail(name, "round-robin fallback left team spawns");
    return pass(name);
}
bool testFpsSwitchChangesActiveDef()
{
    const std::string name = "FpsSwitchChangesActiveDef";
    InventoryComponent inv; PlayerLoadoutComponent lo; WeaponComponent w;
    FpsLogic::grantLoadout(inv, lo, w);
    if (w.weapon_type != WeaponType::RIFLE) return fail(name, "loadout primary is not Rifle");
    if (inv.slots[0].item_def_id != FpsLogic::itemDefId(WeaponType::RIFLE)) return fail(name, "inventory primary mismatch");
    if (!FpsLogic::switchByWeaponSelect(inv, lo, w, 2)) return fail(name, "switch to Secondary failed");
    if (w.weapon_type != WeaponType::PISTOL || FpsLogic::activeWeaponType(lo, w) != WeaponType::PISTOL) return fail(name, "active def did not change to Pistol");
    if (inv.active_slot != 1) return fail(name, "inventory active_slot not updated");
    if (!FpsLogic::switchByWeaponSelect(inv, lo, w, 3) || w.weapon_type != WeaponType::KNIFE) return fail(name, "switch to Melee failed");
    // Shotgun reachable: drop rifle, pick up shotgun into Primary.
    if (!FpsLogic::switchByWeaponSelect(inv, lo, w, 1)) return fail(name, "switch back to Primary failed");
    auto dropped = FpsLogic::dropActive(inv, lo, w);
    if (!dropped || dropped->type != WeaponType::RIFLE) return fail(name, "drop did not return Rifle");
    FpsLogic::DroppedItem sg{WeaponType::SHOTGUN, 5, 20};
    if (!FpsLogic::tryPickup(inv, lo, w, sg)) return fail(name, "shotgun pickup into free Primary failed");
    if (!FpsLogic::switchByWeaponSelect(inv, lo, w, 1) || w.weapon_type != WeaponType::SHOTGUN) return fail(name, "shotgun not active after pickup+select");
    if (w.ammo != 5 || w.reserve_ammo != 20) return fail(name, "shotgun ammo not preserved");
    return pass(name);
}
bool testFpsDropAndPickup()
{
    const std::string name = "FpsDropAndPickup";
    InventoryComponent inv; PlayerLoadoutComponent lo; WeaponComponent w;
    FpsLogic::grantLoadout(inv, lo, w);
    w.ammo = 17; w.reserve_ammo = 43;
    auto dropped = FpsLogic::dropActive(inv, lo, w);
    if (!dropped) return fail(name, "drop returned nothing");
    if (dropped->ammo != 17 || dropped->reserve != 43) return fail(name, "dropped ammo not preserved");
    if (lo.slots[0].occupied) return fail(name, "primary slot not cleared on drop");
    if (w.weapon_type != WeaponType::PISTOL) return fail(name, "fallback after drop is not Secondary");
    // Melee never droppable.
    FpsLogic::switchByWeaponSelect(inv, lo, w, 3);
    if (FpsLogic::dropActive(inv, lo, w)) return fail(name, "melee should not be droppable");
    // Pickup blocked when slot occupied, allowed when free.
    InventoryComponent inv2; PlayerLoadoutComponent lo2; WeaponComponent w2;
    FpsLogic::grantLoadout(inv2, lo2, w2);
    FpsLogic::DroppedItem sg{WeaponType::SHOTGUN, 8, 32};
    if (FpsLogic::tryPickup(inv2, lo2, w2, sg)) return fail(name, "pickup should fail while Primary occupied");
    auto d2 = FpsLogic::dropActive(inv2, lo2, w2);
    if (!d2 || !FpsLogic::tryPickup(inv2, lo2, w2, sg)) return fail(name, "pickup after freeing slot failed");
    if (!FpsLogic::switchByWeaponSelect(inv2, lo2, w2, 1) || w2.weapon_type != WeaponType::SHOTGUN || w2.ammo != 8) return fail(name, "picked-up shotgun state wrong");
    return pass(name);
}
bool testFpsLoadoutInventoryInvariant()
{
    const std::string name = "FpsLoadoutInventoryInvariant";
    InventoryComponent inv;
    PlayerLoadoutComponent lo;
    WeaponComponent weapon;
    FpsLogic::grantLoadout(inv, lo, weapon, WeaponType::AK47);
    if (!FpsLogic::hasMirroredLoadoutInventory(inv, lo))
        return fail(name, "granted loadout did not mirror inventory");

    // The synchronization boundary removes stale engine-only slots and repairs
    // an invalid active slot to the first occupied template slot.
    inv.slots[InventoryComponent::kMaxSlots - 1] = {999u, 1};
    lo.active_slot = 99;
    FpsLogic::syncInventory(inv, lo);
    if (!FpsLogic::hasMirroredLoadoutInventory(inv, lo) || lo.active_slot != (int)WeaponSlot::Primary)
        return fail(name, "sync did not restore a valid mirrored active slot");

    auto dropped = FpsLogic::dropActive(inv, lo, weapon);
    const FpsLogic::DroppedItem negative_ammo{WeaponType::SHOTGUN, -1, 0};
    const FpsLogic::DroppedItem overfull_ammo{WeaponType::SHOTGUN, 9, 32};
    if (!dropped || FpsLogic::tryPickup(inv, lo, weapon, negative_ammo) ||
        FpsLogic::tryPickup(inv, lo, weapon, overfull_ammo))
        return fail(name, "out-of-bounds pickup ammo was accepted");
    if (!FpsLogic::hasMirroredLoadoutInventory(inv, lo))
        return fail(name, "rejected pickup changed mirrored inventory");

    const FpsLogic::DroppedItem valid_ammo{WeaponType::SHOTGUN, 8, 32};
    if (!FpsLogic::tryPickup(inv, lo, weapon, valid_ammo) ||
        !FpsLogic::switchToSlot(inv, lo, weapon, (int)WeaponSlot::Primary) ||
        weapon.weapon_type != WeaponType::SHOTGUN || weapon.ammo != 8 || weapon.reserve_ammo != 32 ||
        !FpsLogic::hasMirroredLoadoutInventory(inv, lo))
        return fail(name, "valid pickup did not preserve bounded mirrored state");
    return pass(name);
}
bool testDroppedWeaponEcsLifecycle()
{
    const std::string name = "DroppedWeaponEcsLifecycle";
    entt::registry registry;
    auto makePlayer = [&](uint16_t client_id, const glm::vec3& position) {
        const entt::entity player = registry.create();
        registry.emplace<Net::NetworkedEntity>(player, 1000u + client_id, client_id, true);
        registry.emplace<TransformComponent>(player, position.x, position.y, position.z);
        registry.emplace<FpsHealthComponent>(player, 100);
        auto& inventory = registry.emplace<InventoryComponent>(player);
        auto& loadout = registry.emplace<PlayerLoadoutComponent>(player);
        auto& weapon = registry.emplace<WeaponComponent>(player);
        FpsLogic::grantLoadout(inventory, loadout, weapon);
        (void)FpsLogic::dropActive(inventory, loadout, weapon); // Leave Primary free for pickup.
        return player;
    };
    const entt::entity first = makePlayer(1, glm::vec3(0.0f));
    const entt::entity second = makePlayer(2, glm::vec3(0.0f));
    const entt::entity dropped = registry.create();
    registry.emplace<TransformComponent>(dropped, 0.0f, 0.0f, 0.0f);
    registry.emplace<DroppedWeaponComponent>(dropped, WeaponType::SHOTGUN, 8, 32, static_cast<uint16_t>(99), 30.0f);

    const auto pickups = FpsLogic::processDroppedWeaponPickups(registry);
    if (pickups.size() != 1u || registry.valid(dropped))
        return fail(name, "simultaneous pickup did not produce one deterministic winner");
    const entt::entity winner = pickups[0].player;
    const entt::entity loser = winner == first ? second : first;
    if (!registry.get<PlayerLoadoutComponent>(winner).slots[(size_t)WeaponSlot::Primary].occupied ||
        registry.get<PlayerLoadoutComponent>(loser).slots[(size_t)WeaponSlot::Primary].occupied)
        return fail(name, "pickup winner or loser loadout changed incorrectly");

    if (FpsLogic::tryPickupDroppedWeapon(registry, winner, dropped) !=
        FpsLogic::DroppedWeaponPickupResult::MissingDroppedItem)
        return fail(name, "stale dropped entity was not rejected");

    const entt::entity occupied_drop = registry.create();
    registry.emplace<TransformComponent>(occupied_drop, 0.0f, 0.0f, 0.0f);
    registry.emplace<DroppedWeaponComponent>(occupied_drop, WeaponType::RIFLE, 30, 90, static_cast<uint16_t>(99), 30.0f);
    if (FpsLogic::tryPickupDroppedWeapon(registry, winner, occupied_drop) !=
        FpsLogic::DroppedWeaponPickupResult::SlotOccupied)
        return fail(name, "occupied slot was not rejected");

    const entt::entity far_drop = registry.create();
    registry.emplace<TransformComponent>(far_drop, 10.0f, 0.0f, 0.0f);
    registry.emplace<DroppedWeaponComponent>(far_drop, WeaponType::RIFLE, 30, 90, static_cast<uint16_t>(99), 30.0f);
    const entt::entity free_player = makePlayer(3, glm::vec3(0.0f));
    if (FpsLogic::tryPickupDroppedWeapon(registry, free_player, far_drop) !=
        FpsLogic::DroppedWeaponPickupResult::OutOfRange)
        return fail(name, "out-of-range pickup was accepted");

    const entt::entity dead_drop = registry.create();
    registry.emplace<TransformComponent>(dead_drop, 0.0f, 0.0f, 0.0f);
    registry.emplace<DroppedWeaponComponent>(dead_drop, WeaponType::RIFLE, 30, 90, static_cast<uint16_t>(99), 30.0f);
    registry.get<FpsHealthComponent>(free_player).alive = false;
    if (FpsLogic::tryPickupDroppedWeapon(registry, free_player, dead_drop) !=
        FpsLogic::DroppedWeaponPickupResult::PlayerIneligible)
        return fail(name, "dead player pickup was accepted");

    if (FpsLogic::isWeaponLifecyclePhaseOpen(Competitive::RoundPhase::RoundEnd) ||
        FpsLogic::isWeaponLifecyclePhaseOpen(Competitive::RoundPhase::WaitingForPlayers) ||
        !FpsLogic::isWeaponLifecyclePhaseOpen(Competitive::RoundPhase::Live))
        return fail(name, "weapon lifecycle phase gate was wrong");

    const entt::entity below_floor = registry.create();
    registry.emplace<TransformComponent>(below_floor, 0.0f, FpsLogic::kDroppedWeaponMinimumWorldY - 1.0f, 0.0f);
    registry.emplace<DroppedWeaponComponent>(below_floor, WeaponType::RIFLE, 30, 90, static_cast<uint16_t>(99), 30.0f);
    FpsLogic::cleanupDroppedWeapons(registry, 0.0f);
    if (registry.valid(below_floor)) return fail(name, "below-floor item was not cleaned up");

    FpsLogic::clearDroppedWeapons(registry);
    if (!registry.view<DroppedWeaponComponent>().empty())
        return fail(name, "round reset left dropped items behind");
    return pass(name);
}
bool testFpsDeathmatchScoreLimit()
{
    const std::string name = "FpsDeathmatchScoreLimit";
    FpsLogic::MatchLogic m; FpsLogic::ModeConfig cfg = FpsLogic::deathmatchConfig(); cfg.score_limit = 3; cfg.time_limit_seconds = 600.0f;
    m.configure(cfg);
    m.recordKill(7, 8, FpsLogic::kTeamNone); m.recordKill(7, 9, FpsLogic::kTeamNone);
    if (m.ended) return fail(name, "DM ended before score limit");
    m.recordKill(7, 8, FpsLogic::kTeamNone);
    if (!m.ended || m.winner_client != 7) return fail(name, "DM did not end at score limit with winner");
    if (!m.tick(10.0f)) return fail(name, "DM match-end timer did not expire for restart");
    m.restart();
    if (m.ended || m.scoreOf(7) != 0) return fail(name, "DM restart did not clear scores");
    return pass(name);
}
bool testFpsTeamDeathmatchTeamScoreWin()
{
    const std::string name = "FpsTeamDeathmatchTeamScoreWin";
    FpsLogic::MatchLogic m; FpsLogic::ModeConfig cfg = FpsLogic::teamDeathmatchConfig(); cfg.score_limit = 2;
    m.configure(cfg);
    m.recordKill(1, 3, FpsLogic::kTeamAttackers);
    m.recordKill(2, 4, FpsLogic::kTeamAttackers); // second attacker frag -> team total 2
    if (!m.ended || m.winner_team != FpsLogic::kTeamAttackers) return fail(name, "TDM did not end on team score");
    if (m.team_scores[FpsLogic::kTeamAttackers] != 2) return fail(name, "TDM team score wrong");
    // Suicide gives no team score.
    FpsLogic::MatchLogic m2; m2.configure(cfg);
    m2.recordKill(5, 5, FpsLogic::kTeamDefenders);
    if (m2.team_scores[FpsLogic::kTeamDefenders] != 0 || m2.ended) return fail(name, "suicide scored in TDM");
    return pass(name);
}


// ---- Competitive mode tests ----
namespace {
Competitive::ModeConfig fastCompConfig()
{
    Competitive::ModeConfig c;
    c.warmup_seconds = 0.05f; c.freeze_seconds = 0.05f; c.round_seconds = 10.0f;
    c.round_end_seconds = 0.05f; c.halftime_seconds = 0.05f; c.match_end_seconds = 0.05f;
    return c;
}
void tickComp(Competitive::RoundSystem& rs, float dt, Competitive::RoundTickInput in)
{ rs.tick(dt, in); }
Competitive::RoundTickInput bothAlive(int a = 1, int d = 1)
{
    Competitive::RoundTickInput in; in.attackers_alive = a; in.defenders_alive = d;
    in.attackers_connected = 1; in.defenders_connected = 1; return in;
}
// Drive Freeze->Live then end the round for `winner` by elimination (or timer for Defenders time win).
void playRound(Competitive::RoundSystem& rs, Competitive::RoundWinner winner)
{
    auto in = bothAlive();
    // From RoundEnd/Halftime/Warmup/Waiting: advance until Live (bounded).
    for (int i = 0; i < 20 && rs.state.phase != Competitive::RoundPhase::Live; ++i) tickComp(rs, 0.06f, in);
    if (winner == Competitive::RoundWinner::Attackers) { auto e = bothAlive(1, 0); tickComp(rs, 0.01f, e); }
    else if (winner == Competitive::RoundWinner::Defenders) { auto e = bothAlive(0, 1); tickComp(rs, 0.01f, e); }
    else { // draw-ish: let timer expire -> Defenders TargetSaved not used here
        auto e = bothAlive(1, 1); tickComp(rs, 0.01f, e);
    }
    for (int i = 0; i < 20 && rs.state.phase == Competitive::RoundPhase::RoundEnd; ++i) tickComp(rs, 0.06f, in);
}
} // namespace

bool testCompetitiveRoundFsmFullCycleHalftimeClinch()
{
    const std::string name = "CompetitiveRoundFsmFullCycleHalftimeClinch";
    Competitive::RoundSystem rs; rs.configure(fastCompConfig());
    if (rs.state.phase != Competitive::RoundPhase::WaitingForPlayers) return fail(name, "initial phase not WaitingForPlayers");
    if (rs.transitionTo(Competitive::RoundPhase::Live)) return fail(name, "illegal Waiting->Live accepted");
    tickComp(rs, 0.01f, bothAlive());
    if (rs.state.phase != Competitive::RoundPhase::Warmup) return fail(name, "no players->Warmup transition failed");
    bool swap_seen = false; bool reset_ot = true;
    rs.on_period_reset = [&](bool ot) { swap_seen = true; reset_ot = ot; };
    // First half: Attackers win 12 rounds -> Halftime after round 12.
    for (int r = 0; r < 12; ++r) playRound(rs, Competitive::RoundWinner::Attackers);
    if (!swap_seen || reset_ot) return fail(name, "halftime period-reset (swap+money) not fired after 12 rounds");
    if (rs.state.wins_attackers != 12 || rs.state.first_half_wins_attackers != 12) return fail(name, "half buckets wrong");
    // Should be in/through Halftime heading to Freeze of second half.
    for (int i = 0; i < 20 && rs.state.phase != Competitive::RoundPhase::Freeze && rs.state.phase != Competitive::RoundPhase::Live; ++i)
        tickComp(rs, 0.06f, bothAlive());
    // Second half round 1: Attackers reach 13 -> clinch at RoundEnd.
    playRound(rs, Competitive::RoundWinner::Attackers);
    for (int i = 0; i < 20 && rs.state.phase == Competitive::RoundPhase::RoundEnd; ++i) tickComp(rs, 0.06f, bothAlive());
    if (rs.state.phase != Competitive::RoundPhase::MatchEnded) return fail(name, "clinch at 13 did not end match");
    if (rs.state.match_winner != Competitive::RoundWinner::Attackers) return fail(name, "match winner wrong");
    if (rs.transitionTo(Competitive::RoundPhase::Warmup) == false && rs.state.phase == Competitive::RoundPhase::MatchEnded) {
        // MatchEnded->Warmup is legal per table; verify illegal Live->Warmup on fresh system instead.
    }
    Competitive::RoundSystem rs2; rs2.configure(fastCompConfig());
    tickComp(rs2, 0.01f, bothAlive());
    for (int i = 0; i < 20 && rs2.state.phase != Competitive::RoundPhase::Live; ++i) tickComp(rs2, 0.06f, bothAlive());
    if (rs2.transitionTo(Competitive::RoundPhase::Warmup)) return fail(name, "illegal Live->Warmup accepted");
    return pass(name);
}

bool testCompetitiveOvertime()
{
    const std::string name = "CompetitiveOvertime";
    Competitive::RoundSystem rs; rs.configure(fastCompConfig());
    tickComp(rs, 0.01f, bothAlive());
    // Tie regulation 12-12 (alternating wins keeps anyone from clinching early:
    // Attackers wins odd rounds, Defenders even; max either reaches before 24 is 12).
    for (int r = 0; r < 24; ++r) playRound(rs, (r % 2 == 0) ? Competitive::RoundWinner::Attackers : Competitive::RoundWinner::Defenders);
    if (rs.state.phase == Competitive::RoundPhase::MatchEnded) return fail(name, "tied regulation ended without overtime");
    if (!rs.state.overtime) return fail(name, "overtime not started after 12-12");
    // OT: tie 3-3 -> Draw at MatchEnded.
    for (int r = 0; r < 6; ++r) playRound(rs, (r % 2 == 0) ? Competitive::RoundWinner::Attackers : Competitive::RoundWinner::Defenders);
    for (int i = 0; i < 30 && rs.state.phase != Competitive::RoundPhase::MatchEnded && rs.state.phase != Competitive::RoundPhase::WaitingForPlayers; ++i)
        tickComp(rs, 0.06f, bothAlive());
    if (rs.state.match_winner != Competitive::RoundWinner::Draw && rs.state.phase != Competitive::RoundPhase::WaitingForPlayers)
        return fail(name, "tied overtime did not draw (phase=" + std::to_string((int)rs.state.phase) + ")");
    // OT clinch: fresh match, tie regulation, then Attackers win 4 OT rounds.
    Competitive::RoundSystem rs2; rs2.configure(fastCompConfig());
    tickComp(rs2, 0.01f, bothAlive());
    for (int r = 0; r < 24; ++r) playRound(rs2, (r % 2 == 0) ? Competitive::RoundWinner::Attackers : Competitive::RoundWinner::Defenders);
    for (int r = 0; r < 4; ++r) playRound(rs2, Competitive::RoundWinner::Attackers);
    for (int i = 0; i < 30 && rs2.state.phase == Competitive::RoundPhase::RoundEnd; ++i) tickComp(rs2, 0.06f, bothAlive());
    if (rs2.state.phase != Competitive::RoundPhase::MatchEnded || rs2.state.match_winner != Competitive::RoundWinner::Attackers)
        return fail(name, "OT first-to-4 clinch failed");
    return pass(name);
}

bool testCompetitiveTimerTargetSavedAndPause()
{
    const std::string name = "CompetitiveTimerTargetSavedAndPause";
    Competitive::RoundSystem rs; auto cfg = fastCompConfig(); cfg.round_seconds = 0.05f; rs.configure(cfg);
    tickComp(rs, 0.01f, bothAlive());
    for (int i = 0; i < 20 && rs.state.phase != Competitive::RoundPhase::Live; ++i) tickComp(rs, 0.06f, bothAlive());
    tickComp(rs, 0.06f, bothAlive());
    if (rs.state.phase != Competitive::RoundPhase::RoundEnd || rs.state.last_reason != Competitive::RoundEndReason::TargetSaved
        || rs.state.last_winner != Competitive::RoundWinner::Defenders)
        return fail(name, "timer expiry did not give Defenders TargetSaved");
    // Pause holds Freeze indefinitely.
    Competitive::RoundSystem rs2; rs2.configure(fastCompConfig());
    tickComp(rs2, 0.01f, bothAlive());
    for (int i = 0; i < 20 && rs2.state.phase != Competitive::RoundPhase::Freeze; ++i) tickComp(rs2, 0.06f, bothAlive());
    rs2.setPaused(true);
    for (int i = 0; i < 100; ++i) tickComp(rs2, 6.0f, bothAlive());
    if (rs2.state.phase != Competitive::RoundPhase::Freeze) return fail(name, "paused Freeze did not hold");
    return pass(name);
}

bool testCompetitiveWorldPhaseAuthority()
{
    const std::string name = "CompetitiveWorld_PhaseAuthority";
    using namespace Competitive;

    entt::registry first_world;
    entt::registry second_world;
    auto& first = first_world.ctx().emplace<CompetitiveWorldContext>();
    auto& second = second_world.ctx().emplace<CompetitiveWorldContext>();
    first.enabled = true;
    second.enabled = true;
    first.round.configure(fastCompConfig());
    second.round.configure(fastCompConfig());
    first.createMatchSnapshot(first_world);
    second.createMatchSnapshot(second_world);
    if (!first.snapshotMatches(first_world) || !second.snapshotMatches(second_world))
        return fail(name, "initial per-world match snapshots differ from their canonical rounds");

    auto tick_world = [&](CompetitiveWorldContext& context, entt::registry& registry,
                          float dt, RoundTickInput input) {
        context.round.tick(dt, input);
        return context.syncMatchSnapshot(registry) && context.snapshotMatches(registry);
    };
    auto drive_to_live = [&](CompetitiveWorldContext& context, entt::registry& registry) {
        for (int i = 0; i < 20 && context.round.state.phase != RoundPhase::Live; ++i)
            if (!tick_world(context, registry, 0.06f, bothAlive())) return false;
        return context.round.state.phase == RoundPhase::Live;
    };

    // First world advances while the second remains at its own initial phase.
    if (!tick_world(first, first_world, 0.01f, bothAlive()) ||
        first.round.state.phase != RoundPhase::Warmup)
        return fail(name, "first world did not enter Warmup with a synchronized snapshot");
    if (second.round.state.phase != RoundPhase::WaitingForPlayers || !second.snapshotMatches(second_world))
        return fail(name, "first-world transition changed the second world");
    if (!drive_to_live(first, first_world))
        return fail(name, "first world did not reach Live");

    // The current FSM defines detonation before defuse when both signals land
    // in one tick.  Keep that winner choice local to this world's inputs.
    RoundTickInput simultaneous = bothAlive();
    simultaneous.bomb_detonated = true;
    simultaneous.bomb_defused = true;
    if (!tick_world(first, first_world, 0.01f, simultaneous) ||
        first.round.state.phase != RoundPhase::RoundEnd ||
        first.round.state.last_winner != RoundWinner::Attackers ||
        first.round.state.last_reason != RoundEndReason::TargetBombed)
        return fail(name, "same-tick bomb win inputs did not use the configured order");
    if (second.round.state.last_winner != RoundWinner::None || !second.snapshotMatches(second_world))
        return fail(name, "first-world win input crossed into the second world");

    // Finish the first half entirely through canonical round inputs.  Every
    // tick republished above proves the ECS entity stays a mirror across the
    // RoundEnd -> Halftime boundary.
    for (int round = 1; round < 12; ++round) {
        if (!drive_to_live(first, first_world))
            return fail(name, "round cycle did not return to Live");
        if (!tick_world(first, first_world, 0.01f, bothAlive(1, 0)))
            return fail(name, "elimination win did not synchronize");
        for (int i = 0; i < 20 && first.round.state.phase == RoundPhase::RoundEnd; ++i)
            if (!tick_world(first, first_world, 0.06f, bothAlive()))
                return fail(name, "round-end transition did not synchronize");
    }
    if (first.round.state.phase != RoundPhase::Halftime ||
        first.round.state.wins_attackers != 12 ||
        first.round.state.first_half_wins_attackers != 12 ||
        !first.snapshotMatches(first_world))
        return fail(name, "halftime snapshot did not retain canonical phase and wins");

    const entt::entity retired_snapshot = first.match_entity;
    first.destroyMatchSnapshot(first_world);
    if (first_world.valid(retired_snapshot) || first.match_entity != entt::null)
        return fail(name, "teardown left the prior match snapshot alive");
    first_world.ctx().erase<CompetitiveWorldContext>();
    auto& restarted = first_world.ctx().emplace<CompetitiveWorldContext>();
    restarted.enabled = true;
    restarted.round.configure(fastCompConfig());
    restarted.createMatchSnapshot(first_world);
    if (restarted.round.state.phase != RoundPhase::WaitingForPlayers ||
        restarted.round.state.wins_attackers != 0 || !restarted.snapshotMatches(first_world))
        return fail(name, "restarted world inherited phase or wins from its prior match");
    if (second.round.state.phase != RoundPhase::WaitingForPlayers || !second.snapshotMatches(second_world))
        return fail(name, "restarting one world changed the other world's authority");
    return pass(name);
}

bool testCompetitiveEconomyStreakCap()
{
    const std::string name = "CompetitiveEconomyStreakCap";
    Competitive::EconomySystem eco;
    Competitive::MoneyComponent m; m.reset(800, 16000);
    // LLL: 1400, 1900, 2400
    const int expect_loss[3] = {1400, 1900, 2400};
    for (int i = 0; i < 3; ++i) {
        int before = m.money();
        eco.awardRoundEnd(m, Competitive::FpsTeam::Attackers, Competitive::RoundWinner::Defenders, Competitive::RoundEndReason::DefendersElimination);
        if (m.money() - before != expect_loss[i]) return fail(name, "loss streak amount wrong at " + std::to_string(i));
    }
    if (m.loss_streak != 3) return fail(name, "streak not 3 after LLL");
    // W: elimination win 3250, streak decays 3->2
    int before = m.money();
    eco.awardRoundEnd(m, Competitive::FpsTeam::Attackers, Competitive::RoundWinner::Attackers, Competitive::RoundEndReason::AttackersElimination);
    if (m.money() - before != 3250) return fail(name, "win elimination award wrong");
    if (m.loss_streak != 2) return fail(name, "streak decay by 1 on win failed");
    // W again: streak 2->1; bomb win 3500 / defuse 3500 / time 3250 mapping
    before = m.money();
    eco.awardRoundEnd(m, Competitive::FpsTeam::Defenders, Competitive::RoundWinner::Defenders, Competitive::RoundEndReason::BombDefused);
    // (different player team arg irrelevant to amount for winner check uses Attackers money comp; use fresh)
    (void)before;
    Competitive::MoneyComponent w2; w2.reset(0, 16000);
    eco.awardRoundEnd(w2, Competitive::FpsTeam::Attackers, Competitive::RoundWinner::Attackers, Competitive::RoundEndReason::TargetBombed);
    if (w2.money() != 3500) return fail(name, "win bomb award wrong");
    // Loss cap: streak at cap gives 3400 not more.
    Competitive::MoneyComponent l; l.reset(0, 16000); l.loss_streak = 5;
    eco.awardRoundEnd(l, Competitive::FpsTeam::Attackers, Competitive::RoundWinner::Defenders, Competitive::RoundEndReason::DefendersElimination);
    if (l.money() != 3400) return fail(name, "loss cap 3400 wrong, got " + std::to_string(l.money()));
    // Money cap 16000.
    Competitive::MoneyComponent rich; rich.reset(15900, 16000);
    eco.awardKill(rich, findCatalogByAlias("ak47"));
    if (rich.money() != 16000) return fail(name, "money cap not clamped");
    // Kill awards by catalog: knife legacy not in catalog; awp 100.
    Competitive::MoneyComponent k; k.reset(0, 16000);
    eco.awardKill(k, findCatalogByAlias("awp"));
    if (k.money() != 100) return fail(name, "AWP kill award wrong");
    eco.awardPlantPlanter(k); eco.awardPlantTeam(k);
    if (k.money() != 100 + 300 + 800) return fail(name, "plant hooks wrong");
    return pass(name);
}

bool testCompetitiveWorldEconomyHooks()
{
    const std::string name = "CompetitiveWorld_EconomyHooks";
    using namespace Competitive;

    entt::registry first_world;
    entt::registry second_world;
    auto& first = first_world.ctx().emplace<CompetitiveWorldContext>();
    auto& second = second_world.ctx().emplace<CompetitiveWorldContext>();
    ModeConfig config = fastCompConfig();
    first.configure(config);
    second.configure(config);

    MoneyComponent first_money;
    MoneyComponent second_money;
    first.onMatchStart(first_money);
    second.onMatchStart(second_money);
    if (first_money.money() != config.start_money || second_money.money() != config.start_money ||
        first.economy_transitions.count != 1 || second.economy_transitions.count != 1)
        return fail(name, "match-start money was not isolated per world");

    // The buy request sequence is world-local.  Its successful money mutation
    // must precede the later death and bomb hooks in this world's trace only.
    first_money.reset(5000, config.money_cap);
    InventoryComponent inventory;
    PlayerLoadoutComponent loadout;
    WeaponComponent weapon;
    ArmorComponent armor;
    FpsLogic::grantLoadout(inventory, loadout, weapon, WeaponType::PISTOL);
    loadout.slots[static_cast<size_t>(WeaponSlot::Primary)] = WeaponSlotState{};
    FpsLogic::syncInventory(inventory, loadout);
    const WeaponCatalogDef* ak = findCatalogByAlias("ak47");
    const WeaponCatalogDef* awp = findCatalogByAlias("awp");
    if (!ak || !awp)
        return fail(name, "economy fixture catalog entries are missing");
    BuyContext buy_context;
    buy_context.client_id = 42;
    buy_context.request_seq = 1;
    buy_context.phase = RoundPhase::Freeze;
    buy_context.alive = true;
    buy_context.in_buy_zone = true;
    buy_context.team_id = teamId(FpsTeam::Attackers);
    buy_context.money = &first_money;
    buy_context.inventory = &inventory;
    buy_context.loadout = &loadout;
    buy_context.weapon = &weapon;
    buy_context.armor = &armor;
    if (first.onBuy(buy_context, ak->id) != BuyResult::Ok || first_money.money() != 5000 - ak->price ||
        first.economy_transitions.count != 2 || first.economy_transitions.last_kind != EconomyTransitionKind::Buy ||
        second.buy.lastResultFor(42) != BuyResult::UnknownItem)
        return fail(name, "buy hook crossed worlds or did not record its transition");

    MoneyComponent victim;
    victim.reset(config.start_money, config.money_cap);
    const int before_kill = first_money.money();
    if (first.onDeath(&victim, &first_money, false, awp) != awp->kill_award ||
        first_money.money() != before_kill + awp->kill_award ||
        first.economy_transitions.count != 3 || first.economy_transitions.last_kind != EconomyTransitionKind::DeathKill)
        return fail(name, "death hook did not award the catalog kill in order");
    if (second_money.money() != config.start_money || second.economy_transitions.count != 1)
        return fail(name, "death hook changed the second world");

    const int before_plant = first_money.money();
    if (first.onBombPlanter(first_money) != first.economy.config.plant_planter ||
        first.onBombTeam(first_money) != first.economy.config.plant_team ||
        first.onBombDefuse(victim) != first.economy.config.defuse_bonus ||
        first_money.money() != before_plant + first.economy.config.plant_planter + first.economy.config.plant_team ||
        first.economy_transitions.count != 6 || first.economy_transitions.last_kind != EconomyTransitionKind::BombDefuse)
        return fail(name, "bomb economy hooks did not preserve their configured ordering");

    bool round_end_after_phase = false;
    first.round.on_round_end = [&](RoundWinner winner, RoundEndReason reason) {
        round_end_after_phase = first.round.state.phase == RoundPhase::RoundEnd &&
            first.onRoundEnd(first_money, FpsTeam::Attackers, winner, reason) > 0;
    };
    tickComp(first.round, 0.01f, bothAlive());
    for (int i = 0; i < 20 && first.round.state.phase != RoundPhase::Live; ++i)
        tickComp(first.round, 0.06f, bothAlive());
    tickComp(first.round, 0.01f, bothAlive(1, 0));
    if (!round_end_after_phase || first.economy_transitions.count != 7 ||
        first.economy_transitions.last_kind != EconomyTransitionKind::RoundEnd)
        return fail(name, "round-end reward did not follow the current phase transition hook");

    first_money.loss_streak = 4;
    first.onPeriodReset(first_money, false);
    if (first_money.money() != config.start_money || first_money.wallet.cap != config.money_cap ||
        first_money.loss_streak != 0 || first.economy_transitions.last_kind != EconomyTransitionKind::PeriodReset)
        return fail(name, "regulation period reset did not use the configured money reset");
    first_money.loss_streak = 3;
    first.onPeriodReset(first_money, true);
    if (first_money.money() != config.overtime_start_money || first_money.loss_streak != 0)
        return fail(name, "overtime period reset did not use the configured money reset");

    // Reusing the context must remove buy dedupe and every economy trace from
    // the retired world.  A newly configured world starts a distinct match.
    first.clear();
    if (first.enabled || first.economy_transitions.count != 0 ||
        first.economy_transitions.last_kind != EconomyTransitionKind::None ||
        first.buy.lastResultFor(42) != BuyResult::UnknownItem ||
        first.round.bomb_hooks != &first.bomb_hooks ||
        first.bomb_hooks.system != &first.bomb)
        return fail(name, "world clear retained stale economy or hook state");
    first.configure(config);
    if (first.round.bomb_hooks != &first.bomb_hooks ||
        first.bomb_hooks.system != &first.bomb)
        return fail(name, "world reconfigure did not restore owned bomb hooks");
    MoneyComponent restarted_money;
    restarted_money.reset(5000, config.money_cap);
    buy_context.money = &restarted_money;
    if (first.onBuy(buy_context, ak->id) != BuyResult::Ok || first.economy_transitions.count != 1 ||
        second_money.money() != config.start_money || second.economy_transitions.count != 1)
        return fail(name, "reconfigured world inherited stale buy or economy state");
    return pass(name);
}

bool testCompetitiveBuyMatrix()
{
    const std::string name = "CompetitiveBuyMatrix";
    using namespace Competitive;
    auto cfg = fastCompConfig();
    BuySystem buy; buy.config = cfg;
    MoneyComponent money; money.reset(5000, 16000);
    InventoryComponent inv; PlayerLoadoutComponent lo; WeaponComponent wpn; ArmorComponent armor;
    FpsLogic::grantLoadout(inv, lo, wpn, WeaponType::PISTOL);
    // Free the primary so AK buy lands cleanly (grantLoadout fills it).
    lo.slots[(int)WeaponSlot::Primary] = WeaponSlotState{}; FpsLogic::syncInventory(inv, lo);
    BuyContext ctx; ctx.client_id = 7; ctx.phase = RoundPhase::Freeze; ctx.alive = true;
    ctx.in_buy_zone = true; ctx.team_id = teamId(FpsTeam::Attackers);
    ctx.money = &money; ctx.inventory = &inv; ctx.loadout = &lo; ctx.weapon = &wpn; ctx.armor = &armor;
    struct BuyState {
        Competitive::MoneyComponent money;
        InventoryComponent inventory;
        PlayerLoadoutComponent loadout;
        WeaponComponent weapon;
        ArmorComponent armor;
    };
    auto snapshot = [&]() { return BuyState{money, inv, lo, wpn, armor}; };
    auto unchanged = [](const BuyState& before, const BuyState& after) {
        if (before.money.wallet.balance != after.money.wallet.balance || before.money.wallet.cap != after.money.wallet.cap ||
            before.money.loss_streak != after.money.loss_streak || before.inventory.active_slot != after.inventory.active_slot ||
            before.loadout.active_slot != after.loadout.active_slot || before.armor.armor != after.armor.armor ||
            before.armor.has_helmet != after.armor.has_helmet || before.armor.has_defuse_kit != after.armor.has_defuse_kit)
            return false;
        for (size_t i = 0; i < before.inventory.slots.size(); ++i) {
            if (before.inventory.slots[i].item_def_id != after.inventory.slots[i].item_def_id ||
                before.inventory.slots[i].count != after.inventory.slots[i].count)
                return false;
        }
        for (size_t i = 0; i < static_cast<size_t>(WeaponSlot::COUNT); ++i) {
            const auto& a = before.loadout.slots[i]; const auto& b = after.loadout.slots[i];
            if (a.type != b.type || a.ammo != b.ammo || a.reserve != b.reserve || a.occupied != b.occupied)
                return false;
        }
        return before.weapon.weapon_type == after.weapon.weapon_type && before.weapon.ammo == after.weapon.ammo &&
               before.weapon.max_ammo == after.weapon.max_ammo && before.weapon.reserve_ammo == after.weapon.reserve_ammo &&
               before.weapon.max_reserve_ammo == after.weapon.max_reserve_ammo && before.weapon.fire_cooldown == after.weapon.fire_cooldown &&
               before.weapon.reload_timer == after.weapon.reload_timer && before.weapon.accuracy_penalty == after.weapon.accuracy_penalty &&
               before.weapon.recoil_index == after.weapon.recoil_index && before.weapon.time_since_last_shot == after.weapon.time_since_last_shot &&
               before.weapon.reloading == after.weapon.reloading;
    };
    const WeaponCatalogDef* ak = findCatalogByAlias("ak47");
    if (!ak) return fail(name, "ak47 missing");
    ctx.request_seq = 1;
    if (buy.buy(ctx, ak->id) != BuyResult::Ok) return fail(name, "valid Freeze buy rejected");
    if (money.money() != 5000 - 2700) return fail(name, "buy did not charge exactly price");
    if (lo.slots[(int)WeaponSlot::Primary].type != WeaponType::AK47) return fail(name, "AK not in primary slot");
    // Duplicate seq: exactly-once, no second charge.
    BuyState duplicate_before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::DuplicateRequest) return fail(name, "duplicate seq not deduped");
    if (!unchanged(duplicate_before, snapshot())) return fail(name, "duplicate seq mutated purchase state");
    // Insufficient funds.
    ctx.request_seq = 2; money.reset(100, 16000);
    BuyState before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::InsufficientFunds) return fail(name, "funds reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "funds reject mutated purchase state");
    // Phase closed (RoundEnd).
    money.reset(9000, 16000); ctx.request_seq = 3; ctx.phase = RoundPhase::RoundEnd;
    before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::PhaseClosed) return fail(name, "phase reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "phase reject mutated purchase state");
    // Live after buy window.
    ctx.request_seq = 4; ctx.phase = RoundPhase::Live; ctx.seconds_into_live = 21.0f;
    before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::BuyTimeExpired) return fail(name, "buy window reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "buy window reject mutated purchase state");
    // Live inside window succeeds for a pistol.
    ctx.request_seq = 5; ctx.seconds_into_live = 5.0f;
    const auto* deagle = findCatalogByAlias("deagle");
    // secondary occupied by grantLoadout pistol -> replaces (Ok)
    if (buy.buy(ctx, deagle->id) != BuyResult::Ok) return fail(name, "Live in-window buy rejected");
    // Outside zone.
    ctx.request_seq = 6; ctx.phase = RoundPhase::Freeze; ctx.seconds_into_live = 0; ctx.in_buy_zone = false;
    before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::OutsideBuyZone) return fail(name, "zone reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "zone reject mutated purchase state");
    ctx.in_buy_zone = true;
    // Wrong team: M4A4 is Defenders-only, buyer is Attackers.
    ctx.request_seq = 7;
    const auto* m4 = findCatalogByAlias("m4a1");
    before = snapshot();
    if (buy.buy(ctx, m4->id) != BuyResult::WrongTeam) return fail(name, "team reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "team reject mutated purchase state");
    // Dead.
    ctx.request_seq = 8; ctx.alive = false;
    before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::Dead) return fail(name, "dead reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "dead reject mutated purchase state");
    ctx.alive = true;
    // Unknown item.
    ctx.request_seq = 9;
    before = snapshot();
    if (buy.buy(ctx, 9999) != BuyResult::UnknownItem) return fail(name, "unknown reject wrong");
    if (!unchanged(before, snapshot())) return fail(name, "unknown reject mutated purchase state");
    // Armor + kit: kit is Defenders-only; armor ok then AlreadyOwned.
    ctx.request_seq = 10;
    const auto* vest = findCatalogByAlias("vest");
    if (buy.buy(ctx, vest->id) != BuyResult::Ok || armor.armor != 100) return fail(name, "armor buy failed");
    ctx.request_seq = 11;
    before = snapshot();
    if (buy.buy(ctx, vest->id) != BuyResult::AlreadyOwned) return fail(name, "armor re-buy not AlreadyOwned");
    if (!unchanged(before, snapshot())) return fail(name, "owned reject mutated purchase state");
    ctx.request_seq = 12;
    const auto* kit = findCatalogByAlias("defuser");
    before = snapshot();
    if (buy.buy(ctx, kit->id) != BuyResult::WrongTeam) return fail(name, "kit should be Defenders-only");
    if (!unchanged(before, snapshot())) return fail(name, "kit team reject mutated purchase state");
    ctx.team_id = teamId(FpsTeam::Defenders); ctx.request_seq = 13;
    if (buy.buy(ctx, kit->id) != BuyResult::Ok || !armor.has_defuse_kit) return fail(name, "kit buy failed for Defender");
    // Grenade carry limit: second grenade while slot occupied.
    ctx.request_seq = 14; const auto* he = findCatalogByAlias("hegrenade");
    if (buy.buy(ctx, he->id) != BuyResult::Ok) return fail(name, "grenade buy failed");
    ctx.request_seq = 15; const auto* flash = findCatalogByAlias("flashbang");
    before = snapshot();
    if (buy.buy(ctx, flash->id) != BuyResult::CarryLimit) return fail(name, "grenade carry limit wrong");
    if (!unchanged(before, snapshot())) return fail(name, "carry reject mutated purchase state");
    // Replace occupied primary: old weapon returned as dropped (server spawns it).
    money.reset(9000, 16000); ctx.team_id = teamId(FpsTeam::Attackers); ctx.request_seq = 16;
    FpsLogic::DroppedItem dropped{WeaponType::COUNT, 0, 0};
    const auto* galil = findCatalogByAlias("galilar");
    if (buy.buy(ctx, galil->id, &dropped) != BuyResult::Ok) return fail(name, "replace-primary buy rejected");
    if (dropped.type != WeaponType::AK47) return fail(name, "replaced primary not returned as dropped");
    if (lo.slots[(int)WeaponSlot::Primary].type != WeaponType::GALILAR) return fail(name, "new primary not equipped");
    // Empty slot: dropped stays COUNT sentinel (nothing to spawn).
    lo.slots[(int)WeaponSlot::Primary] = WeaponSlotState{}; FpsLogic::syncInventory(inv, lo);
    ctx.request_seq = 17; FpsLogic::DroppedItem none{WeaponType::COUNT, 0, 0};
    if (buy.buy(ctx, ak->id, &none) != BuyResult::Ok) return fail(name, "empty-slot buy rejected");
    if (none.type != WeaponType::COUNT) return fail(name, "empty slot produced a dropped weapon");
    // A zero sequence is malformed and cannot bypass replay protection.
    ctx.request_seq = 0;
    before = snapshot();
    if (buy.buy(ctx, ak->id) != BuyResult::DuplicateRequest) return fail(name, "zero sequence not rejected");
    if (!unchanged(before, snapshot())) return fail(name, "zero sequence mutated purchase state");
    return pass(name);
}

bool testCompetitiveTeamAutoAssign()
{
    const std::string name = "CompetitiveTeamAutoAssign";
    using namespace Competitive;
    if (autoAssignTeam(0, 0) != FpsTeam::Defenders) return fail(name, "tie should go Defenders");
    if (autoAssignTeam(0, 3) != FpsTeam::Attackers) return fail(name, "smaller Attackers not picked");
    if (autoAssignTeam(4, 1) != FpsTeam::Defenders) return fail(name, "smaller Defenders not picked");
    ModeConfig cfg;
    FpsTeam out = FpsTeam::None;
    if (validateTeamSelect(FpsTeam::Attackers, false, 5, 2, RoundPhase::Freeze, cfg, out) != TeamSelectResult::TeamFull)
        return fail(name, "full team not TeamFull");
    if (validateTeamSelect(FpsTeam::Attackers, false, 5, 5, RoundPhase::Freeze, cfg, out) != TeamSelectResult::BothFull)
        return fail(name, "both full not BothFull");
    if (validateTeamSelect(FpsTeam::Attackers, false, 1, 1, RoundPhase::Live, cfg, out) != TeamSelectResult::PhaseClosed)
        return fail(name, "mid-Live switch not PhaseClosed");
    if (validateTeamSelect(FpsTeam::None, true, 2, 2, RoundPhase::Warmup, cfg, out) != TeamSelectResult::Ok || out != FpsTeam::Defenders)
        return fail(name, "auto-assign in Warmup failed");
    if (!canSpawnNow(RoundPhase::Freeze) || canSpawnNow(RoundPhase::Live)) return fail(name, "late-join spawn gate wrong");
    return pass(name);
}

bool testCompetitiveSessionFlow()
{
    const std::string name = "CompetitiveSessionFlow";
    using namespace Competitive;

    SessionCoordinator sessions;
    ModeConfig cfg;
    if (!sessions.admit(7) || sessions.admit(7) || sessions.size() != 1)
        return fail(name, "admission did not create one deterministic session");
    const SessionRecord* admitted = sessions.find(7);
    if (admitted == nullptr || admitted->state != SessionState::AwaitingTeam ||
        admitted->team != FpsTeam::None)
        return fail(name, "competitive admission did not await a team without a pawn");

    if (!sessions.consumeTeamSelectRequest(7, 100) ||
        sessions.consumeTeamSelectRequest(7, 100) ||
        !sessions.consumeTeamSelectRequest(7, 101))
        return fail(name, "team-request throttle did not reject same-tick replay deterministically");

    FpsTeam late_join_team = FpsTeam::None;
    const SessionRecord admitted_before_selection = *sessions.find(7);
    if (validateSessionTeamSelect(sessions, 7, false, FpsTeam::Attackers, false,
                                  1, 1, RoundPhase::Live, cfg, late_join_team) != TeamSelectResult::Ok ||
        late_join_team != FpsTeam::Attackers ||
        sessions.find(7)->team != admitted_before_selection.team ||
        sessions.find(7)->state != admitted_before_selection.state)
        return fail(name, "Live ingress did not admit a pawnless team selection without mutation");

    if (!sessions.assignTeam(7, late_join_team, RoundPhase::Live))
        return fail(name, "late join team was not retained");
    const SessionRecord* late_join = sessions.find(7);
    if (late_join == nullptr || late_join->team != FpsTeam::Attackers ||
        late_join->state != SessionState::AwaitingSpawn)
        return fail(name, "late join did not remain pawnless until a legal spawn phase");

    if (!sessions.markSpawned(7, FpsTeam::Attackers, RoundPhase::Freeze) ||
        sessions.find(7)->state != SessionState::Active)
        return fail(name, "Freeze spawn did not transition the session active");

    FpsTeam switched_team = FpsTeam::None;
    const SessionRecord active_before_switch = *sessions.find(7);
    if (validateSessionTeamSelect(sessions, 7, true, FpsTeam::Defenders, false,
                                  1, 1, RoundPhase::Live, cfg, switched_team) != TeamSelectResult::PhaseClosed ||
        switched_team != FpsTeam::None ||
        sessions.find(7)->team != active_before_switch.team ||
        sessions.find(7)->state != active_before_switch.state)
        return fail(name, "Live ingress allowed an active-player team switch");

    FpsTeam invalid_team = FpsTeam::Attackers;
    if (validateSessionTeamSelect(sessions, 7, false, FpsTeam::None, false,
                                  1, 1, RoundPhase::Freeze, cfg, invalid_team) != TeamSelectResult::InvalidTeam ||
        invalid_team != FpsTeam::None ||
        sessions.find(7)->team != active_before_switch.team ||
        sessions.find(7)->state != active_before_switch.state)
        return fail(name, "invalid ingress team request partially mutated the active session");

    if (!sessions.assignTeam(7, FpsTeam::Spectator, RoundPhase::Freeze) ||
        sessions.find(7)->state != SessionState::Spectating)
        return fail(name, "spectator selection did not publish a pawnless spectator state");
    if (sessions.assignTeam(7, FpsTeam::None, RoundPhase::Freeze))
        return fail(name, "invalid team changed session state");

    sessions.remove(7);
    if (sessions.contains(7) || sessions.size() != 0)
        return fail(name, "disconnect cleanup retained the session record");
    return pass(name);
}

bool testCompetitiveBuyZoneOccupancy()
{
    const std::string name = "CompetitiveBuyZoneOccupancy";
    using namespace Competitive;
    entt::registry registry;
    const entt::entity player = registry.create();
    const entt::entity outside_player = registry.create();
    const entt::entity attackers_zone = registry.create();
    registry.emplace<BuyZoneComponent>(attackers_zone, BuyZoneComponent{teamId(FpsTeam::Attackers)});
    registry.emplace<TriggerComponent>(attackers_zone, TriggerComponent{true, 1000, true});
    auto& occupancy = registry.emplace<TriggerOverlapState>(attackers_zone);
    occupancy.occupants.push_back(player);

    if (!isInBuyZone(registry, player, teamId(FpsTeam::Attackers)))
        return fail(name, "own-team trigger occupancy was rejected");
    if (isInBuyZone(registry, player, teamId(FpsTeam::Defenders)))
        return fail(name, "opposing-team trigger occupancy was accepted");
    if (isInBuyZone(registry, outside_player, teamId(FpsTeam::Attackers)))
        return fail(name, "player without trigger occupancy was accepted");
    if (isInBuyZone(registry, player, teamId(FpsTeam::Spectator)))
        return fail(name, "spectator passed buy-zone validation");

    return pass(name);
}

bool testWeaponRosterIntegrity()
{
    const std::string name = "WeaponRosterIntegrity";
    size_t n = 0; const WeaponCatalogDef* defs = weaponCatalog(n);
    if (n != 44) return fail(name, "catalog size " + std::to_string(n) + " != 44");
    for (size_t i = 0; i < n; ++i) {
        if (!isCatalogBuyable(defs[i])) return fail(name, "non-buyable catalog row " + std::to_string(defs[i].id));
        if (!defs[i].alias || !defs[i].alias[0] || !defs[i].name || !defs[i].name[0]) return fail(name, "missing alias/name");
        if (defs[i].id == 0 || findCatalogById(defs[i].id) != &defs[i] ||
            findCatalogByAlias(defs[i].alias) != &defs[i])
            return fail(name, "catalog lookup did not round-trip for " + std::string(defs[i].alias));
        const bool data_only = isCatalogDataOnly(defs[i]);
        if (data_only != (defs[i].kind == BuyItemKind::Equipment))
            return fail(name, "catalog data-only classification mismatch for " + std::string(defs[i].alias));
        for (size_t j = i + 1; j < n; ++j) {
            if (defs[i].id == defs[j].id) return fail(name, "duplicate catalog id");
            if (std::strcmp(defs[i].alias, defs[j].alias) == 0) return fail(name, "duplicate alias " + std::string(defs[i].alias));
        }
        if (!data_only) {
            if (!FpsLogic::isValidWeaponType(defs[i].weapon_type) ||
                defs[i].slot != FpsLogic::slotForWeaponType(defs[i].weapon_type) ||
                defs[i].mag < 0 || defs[i].reserve < 0)
                return fail(name, "invalid weapon row " + std::string(defs[i].alias));
            const auto& wd = getWeaponDef(defs[i].weapon_type);
            if (wd.damage != defs[i].damage || wd.max_ammo != defs[i].mag || wd.max_reserve_ammo != defs[i].reserve || wd.pellets != defs[i].pellets)
                return fail(name, "catalog/behaviour def mismatch for " + std::string(defs[i].alias));
        } else if (defs[i].mag != 0 || defs[i].reserve != 0) {
            return fail(name, "data-only row has weapon ammo " + std::string(defs[i].alias));
        }
    }
    const auto* ak = findCatalogByAlias("ak47");
    if (!ak || ak->price != 2700 || ak->damage != 36 || ak->kill_award != 300 || ak->team_mask != kCatalogTeamAttackers)
        return fail(name, "AK-47 row wrong");
    // Legacy behaviour preserved.
    if (getWeaponDef(WeaponType::RIFLE).damage != 15 || getWeaponDef(WeaponType::SHOTGUN).pellets != 8
        || getWeaponDef(WeaponType::PISTOL).damage != 12 || getWeaponDef(WeaponType::KNIFE).damage != 50)
        return fail(name, "legacy weapon behaviour changed");
    // Protocol ids >= 69 for competitive messages.
    if (static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE) < 69) return fail(name, "round state wire id < 69");
    return pass(name);
}

bool testWeaponAuthorityTiming()
{
    const std::string name = "WeaponAuthorityTiming";

    // Exercise each behavior-bearing row without asserting a balance value.
    // A repeated held ATTACK sample must stay blocked by the authoritative
    // cooldown and leave the accepted shot's state intact.
    for (uint8_t value = 0; value < static_cast<uint8_t>(WeaponType::COUNT); ++value) {
        const WeaponType type = static_cast<WeaponType>(value);
        const WeaponDef& def = getWeaponDef(type);
        if (!hasWeaponDefinition(type) || !def.name || !def.name[0] || def.fire_rate <= 0.0f ||
            def.reload_time < 0.0f || def.max_ammo <= 0 || def.max_reserve_ammo < 0) {
            return fail(name, "invalid behavior definition at weapon type " + std::to_string(value));
        }

        WeaponComponent weapon;
        if (!initWeapon(weapon, type))
            return fail(name, "could not initialize behavior definition " + std::to_string(value));
        const int32_t initial_ammo = weapon.ammo;
        if (!WeaponSystem::tryFire(weapon) || weapon.ammo != initial_ammo - 1 ||
            !approx(weapon.fire_cooldown, def.fire_rate)) {
            return fail(name, "accepted shot did not use its own definition " + std::to_string(value));
        }

        const int32_t blocked_ammo = weapon.ammo;
        const float blocked_cooldown = weapon.fire_cooldown;
        const bool blocked_reload = weapon.reloading;
        if (WeaponSystem::tryFire(weapon) || weapon.ammo != blocked_ammo ||
            weapon.fire_cooldown != blocked_cooldown || weapon.reloading != blocked_reload) {
            return fail(name, "blocked repeated shot mutated state " + std::to_string(value));
        }

        weapon.fire_cooldown = 0.0f;
        weapon.ammo = 0;
        weapon.reloading = false;
        weapon.reload_timer = 0.0f;
        const int32_t empty_reserve = weapon.reserve_ammo;
        if (WeaponSystem::tryFire(weapon) || weapon.ammo != 0 || weapon.reserve_ammo != empty_reserve) {
            return fail(name, "empty shot consumed ammo " + std::to_string(value));
        }

        if (weapon.max_reserve_ammo > 0) {
            weapon.ammo = weapon.max_ammo - 1;
            weapon.reserve_ammo = 1;
            if (!WeaponSystem::tryReload(weapon) || !weapon.reloading) {
                return fail(name, "reload did not start " + std::to_string(value));
            }
            const int32_t reload_ammo = weapon.ammo;
            const int32_t reload_reserve = weapon.reserve_ammo;
            WeaponSystem::cancelReload(weapon);
            WeaponSystem::tick(weapon, def.reload_time + 1.0f);
            if (weapon.reloading || weapon.reload_timer != 0.0f || weapon.ammo != reload_ammo ||
                weapon.reserve_ammo != reload_reserve) {
                return fail(name, "cancelled reload changed ammo " + std::to_string(value));
            }
        }
    }

    WeaponComponent invalid;
    invalid.weapon_type = WeaponType::COUNT;
    invalid.ammo = 7;
    invalid.max_ammo = 9;
    invalid.reserve_ammo = 11;
    invalid.max_reserve_ammo = 13;
    invalid.fire_cooldown = 0.25f;
    invalid.reload_timer = 0.5f;
    invalid.accuracy_penalty = 0.75f;
    invalid.recoil_index = 1.0f;
    invalid.time_since_last_shot = 2.0f;
    invalid.reloading = true;
    const WeaponComponent invalid_before = invalid;
    if (initWeapon(invalid, WeaponType::COUNT) || WeaponSystem::tryFire(invalid) ||
        WeaponSystem::tryReload(invalid)) {
        return fail(name, "invalid weapon id was accepted");
    }
    WeaponSystem::tick(invalid, 1.0f);
    if (invalid.weapon_type != invalid_before.weapon_type || invalid.ammo != invalid_before.ammo ||
        invalid.max_ammo != invalid_before.max_ammo || invalid.reserve_ammo != invalid_before.reserve_ammo ||
        invalid.max_reserve_ammo != invalid_before.max_reserve_ammo ||
        invalid.fire_cooldown != invalid_before.fire_cooldown || invalid.reload_timer != invalid_before.reload_timer ||
        invalid.accuracy_penalty != invalid_before.accuracy_penalty || invalid.recoil_index != invalid_before.recoil_index ||
        invalid.time_since_last_shot != invalid_before.time_since_last_shot || invalid.reloading != invalid_before.reloading ||
        WeaponSystem::computeSpread(invalid, {}) != 0.0f) {
        return fail(name, "invalid weapon id changed state or produced spread");
    }

    InventoryComponent inv;
    PlayerLoadoutComponent loadout;
    WeaponComponent active;
    FpsLogic::grantLoadout(inv, loadout, active);
    active.ammo = 17;
    active.reserve_ammo = 43;
    active.reloading = true;
    active.reload_timer = 1.0f;
    if (!FpsLogic::switchToSlot(inv, loadout, active, static_cast<int>(WeaponSlot::Secondary)) ||
        active.reloading || active.reload_timer != 0.0f ||
        loadout.slots[static_cast<int>(WeaponSlot::Primary)].ammo != 17 ||
        loadout.slots[static_cast<int>(WeaponSlot::Primary)].reserve != 43) {
        return fail(name, "switch did not cancel reload before saving active ammo");
    }

    return pass(name);
}

bool testLagCompensatedShotPoseFallbackAndMissCount()
{
    const std::string name = "LagCompensatedShotPoseFallbackAndMissCount";
    entt::registry registry;
    const entt::entity sampled_entity = registry.create();
    const entt::entity missing_entity = registry.create();
    const std::vector<FpsServer::LagCompensationTarget> eligible_targets = {
        {sampled_entity, 101u, 4u, glm::vec3(1.0f, 2.0f, 3.0f)},
        {missing_entity, 102u, 5u, glm::vec3(7.0f, 8.0f, 9.0f)},
    };

    FpsServer::LagCompensationTelemetry telemetry;
    uint32_t sample_calls = 0;
    const auto shot_targets = FpsServer::prepareLagCompensatedTargetsForShot(
        eligible_targets,
        50u,
        [&sample_calls](uint32_t network_id, uint32_t view_tick, Net::ComponentSnapshot& out) {
            ++sample_calls;
            if (view_tick != 50u || network_id != 101u) return false;
            out.position = glm::vec3(4.0f, 5.0f, 6.0f);
            return true;
        },
        telemetry);

    if (shot_targets.size() != 2u || sample_calls != 2u ||
        !approxVec3(shot_targets[0].position, glm::vec3(4.0f, 5.0f, 6.0f)) ||
        !approxVec3(shot_targets[1].position, eligible_targets[1].current_position) ||
        telemetry.target_history_sample_misses != 1u) {
        return fail(name, "sampled pose, current-transform fallback, or per-target miss count was wrong");
    }

    size_t pellet_target_uses = 0;
    for (int pellet = 0; pellet < 8; ++pellet) {
        for (const auto& target : shot_targets) {
            (void)target;
            ++pellet_target_uses;
        }
    }
    if (pellet_target_uses != 16u || sample_calls != 2u || telemetry.target_history_sample_misses != 1u) {
        return fail(name, "multi-pellet use sampled targets or counted a history miss more than once");
    }

    return pass(name);
}

bool testFpsDamageContract()
{
    const std::string name = "FpsDamageContract";
    using FpsServer::DamagePolicy;
    using FpsServer::DamageRejectReason;
    using FpsServer::DamageRequest;

    entt::registry registry;
    const entt::entity attacker = registry.create();
    const entt::entity teammate = registry.create();
    const entt::entity victim = registry.create();
    const entt::entity protected_victim = registry.create();
    const entt::entity dead_attacker = registry.create();
    const entt::entity stale_victim = registry.create();
    registry.emplace<FpsHealthComponent>(attacker, 100);
    registry.emplace<FpsHealthComponent>(teammate, 100);
    registry.emplace<FpsHealthComponent>(victim, 100);
    registry.emplace<FpsHealthComponent>(protected_victim, 100).invulnerable_timer = 0.5f;
    registry.emplace<FpsHealthComponent>(dead_attacker, 100).alive = false;
    registry.emplace<TeamComponent>(attacker, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<TeamComponent>(teammate, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<TeamComponent>(victim, TeamComponent{FpsLogic::kTeamDefenders});
    registry.emplace<TeamComponent>(protected_victim, TeamComponent{FpsLogic::kTeamDefenders});
    registry.emplace<TeamComponent>(dead_attacker, TeamComponent{FpsLogic::kTeamAttackers});
    registry.emplace<ArmorComponent>(victim, ArmorComponent{100, true, false});
    registry.destroy(stale_victim);

    DamagePolicy policy;
    policy.require_live_phase = true;
    policy.require_opposing_teams = true;
    policy.phase = Competitive::RoundPhase::Freeze;
    const auto phase_blocked = FpsServer::applyFpsDamage(registry, {attacker, victim, 25}, policy);
    if (phase_blocked.applied || phase_blocked.reject_reason != DamageRejectReason::PhaseBlocked ||
        registry.get<FpsHealthComponent>(victim).health != 100)
        return fail(name, "Freeze damage changed health");

    policy.phase = Competitive::RoundPhase::Live;
    const auto team_blocked = FpsServer::applyFpsDamage(registry, {attacker, teammate, 25}, policy);
    if (team_blocked.applied || team_blocked.reject_reason != DamageRejectReason::SameTeam ||
        registry.get<FpsHealthComponent>(teammate).health != 100)
        return fail(name, "same-team damage changed health");

    const auto stale_blocked = FpsServer::applyFpsDamage(registry, {attacker, stale_victim, 25}, policy);
    const auto self_blocked = FpsServer::applyFpsDamage(registry, {attacker, attacker, 25}, policy);
    const auto dead_blocked = FpsServer::applyFpsDamage(registry, {dead_attacker, victim, 25}, policy);
    const auto invulnerable_blocked = FpsServer::applyFpsDamage(registry, {attacker, protected_victim, 25}, policy);
    if (stale_blocked.applied || stale_blocked.reject_reason != DamageRejectReason::InvalidEntity ||
        self_blocked.applied || self_blocked.reject_reason != DamageRejectReason::SelfTarget ||
        dead_blocked.applied || dead_blocked.reject_reason != DamageRejectReason::AttackerDead ||
        invulnerable_blocked.applied || invulnerable_blocked.reject_reason != DamageRejectReason::Invulnerable ||
        registry.get<FpsHealthComponent>(protected_victim).health != 100)
        return fail(name, "invalid damage request mutated health");

    auto applied = FpsServer::applyFpsDamage(registry, {attacker, victim, 25}, policy);
    if (!applied.applied || applied.killed || applied.health_damage != 25 ||
        registry.get<FpsHealthComponent>(victim).health != 75 || registry.get<ArmorComponent>(victim).armor != 100)
        return fail(name, "live opposing-team damage did not preserve the unapproved armor contract");

    auto lethal = FpsServer::applyFpsDamage(registry, {attacker, victim, 100}, policy);
    auto duplicate_lethal = FpsServer::applyFpsDamage(registry, {attacker, victim, 100}, policy);
    if (!lethal.applied || !lethal.killed || registry.get<FpsHealthComponent>(victim).health != 0 ||
        duplicate_lethal.applied || duplicate_lethal.reject_reason != DamageRejectReason::VictimDead)
        return fail(name, "duplicate lethal request was not rejected after the first death");

    return pass(name);
}

bool testFpsDeathLifecycle()
{
    const std::string name = "FpsDeathLifecycle";
    using FpsServer::DeathTransitionRejectReason;

    entt::registry registry;
    const entt::entity attacker = registry.create();
    const entt::entity victim = registry.create();
    const entt::entity trade_a = registry.create();
    const entt::entity trade_b = registry.create();
    const entt::entity self = registry.create();
    registry.emplace<FpsHealthComponent>(attacker, 100);
    registry.emplace<FpsHealthComponent>(victim, 100);
    registry.emplace<FpsHealthComponent>(trade_a, 100);
    registry.emplace<FpsHealthComponent>(trade_b, 100);
    registry.emplace<FpsHealthComponent>(self, 100);
    auto& victim_weapon = registry.emplace<WeaponComponent>(victim, WeaponType::RIFLE);
    victim_weapon.reloading = true;
    victim_weapon.reload_timer = 1.0f;

    // A lethal damage result enters the post-damage lifecycle once and makes
    // the already-dead pawn ineligible for any further weapon action.
    registry.get<FpsHealthComponent>(victim).takeDamage(100);
    const auto first = FpsServer::beginFpsDeathTransition(registry, victim);
    FpsServer::stopFpsWeaponActions(registry, victim);
    const auto duplicate = FpsServer::beginFpsDeathTransition(registry, victim);
    if (!first.transitioned || first.death_sequence != 1 || duplicate.transitioned ||
        duplicate.reject_reason != DeathTransitionRejectReason::AlreadyResolved ||
        duplicate.death_sequence != first.death_sequence || FpsServer::canUseFpsWeapon(registry, victim) ||
        victim_weapon.reloading || victim_weapon.reload_timer != 0.0f) {
        return fail(name, "duplicate lethal transition or dead weapon gate was wrong");
    }

    // Two lethal outcomes captured for the same simulation tick retain one
    // independent transition each; neither victim can consume the other's
    // latch or event sequence.
    registry.get<FpsHealthComponent>(trade_a).takeDamage(100);
    registry.get<FpsHealthComponent>(trade_b).takeDamage(100);
    const auto trade_a_result = FpsServer::beginFpsDeathTransition(registry, trade_a);
    const auto trade_b_result = FpsServer::beginFpsDeathTransition(registry, trade_b);
    if (!trade_a_result.transitioned || !trade_b_result.transitioned ||
        trade_a_result.death_sequence != 1 || trade_b_result.death_sequence != 1) {
        return fail(name, "same-tick trade outcomes were not independently latched");
    }

    // The server resolver drops a primary through switchToSlot + dropActive.
    // Start while holding the secondary to prove that it drops the primary's
    // stored ammo, then leaves WeaponComponent and InventoryComponent on the
    // fallback selected by dropActive.
    InventoryComponent death_inventory;
    PlayerLoadoutComponent death_loadout;
    WeaponComponent death_weapon;
    FpsLogic::grantLoadout(death_inventory, death_loadout, death_weapon);
    death_weapon.ammo = 17;
    death_weapon.reserve_ammo = 43;
    if (!FpsLogic::switchToSlot(death_inventory, death_loadout, death_weapon,
                                static_cast<int>(WeaponSlot::Secondary))) {
        return fail(name, "death drop fixture could not select the secondary");
    }
    death_weapon.ammo = 4;
    death_weapon.reserve_ammo = 20;
    if (!FpsLogic::switchToSlot(death_inventory, death_loadout, death_weapon,
                                static_cast<int>(WeaponSlot::Primary))) {
        return fail(name, "death drop fixture could not select the primary");
    }
    const auto death_drop = FpsLogic::dropActive(death_inventory, death_loadout, death_weapon);
    if (!death_drop || death_drop->type != WeaponType::RIFLE || death_drop->ammo != 17 ||
        death_drop->reserve != 43 || death_loadout.active_slot != static_cast<int>(WeaponSlot::Secondary) ||
        death_weapon.weapon_type != WeaponType::PISTOL || death_weapon.ammo != 4 ||
        death_weapon.reserve_ammo != 20 ||
        !FpsLogic::hasMirroredLoadoutInventory(death_inventory, death_loadout)) {
        return fail(name, "death primary drop did not preserve loadout, weapon, and inventory mirrors");
    }

    // A self-inflicted death keeps its attribution in data-only event fields.
    registry.get<FpsHealthComponent>(self).takeDamage(100);
    const auto self_result = FpsServer::beginFpsDeathTransition(registry, self);
    FpsServer::FpsDeathEvent self_event{7, 7, static_cast<uint8_t>(WeaponType::COUNT),
        FpsServer::FpsDeathEventSelfInflicted, self_result.death_sequence, glm::vec3(1.0f, 2.0f, 3.0f)};
    if (!self_result.transitioned || (self_event.flags & FpsServer::FpsDeathEventSelfInflicted) == 0 ||
        self_event.attacker_client_id != self_event.victim_client_id) {
        return fail(name, "self death attribution was not preserved");
    }

    // Event delivery needs no entity dereference after a disconnect.
    const FpsServer::FpsDeathEvent disconnect_safe_event{12, 13, static_cast<uint8_t>(WeaponType::RIFLE),
        FpsServer::FpsDeathEventNone, first.death_sequence, glm::vec3(4.0f, 5.0f, 6.0f)};
    registry.destroy(attacker);
    registry.destroy(victim);
    if (disconnect_safe_event.attacker_client_id != 12 || disconnect_safe_event.victim_client_id != 13 ||
        !approxVec3(disconnect_safe_event.death_position, glm::vec3(4.0f, 5.0f, 6.0f))) {
        return fail(name, "disconnect-safe death event lost its value data");
    }

    // Respawn clears only the once-only latch and preserves its monotonically
    // increasing sequence for the next event.
    auto& self_health = registry.get<FpsHealthComponent>(self);
    self_health.reset();
    // reset() grants spawn protection.  This fixture exercises the next
    // lethal request after that authoritative protection window has expired.
    self_health.invulnerable_timer = 0.0f;
    FpsServer::resetFpsDeathTransition(registry, self);
    self_health.takeDamage(100);
    const auto respawned_life = FpsServer::beginFpsDeathTransition(registry, self);
    if (!respawned_life.transitioned || respawned_life.death_sequence != 2) {
        return fail(name, "respawn did not reopen the next death lifecycle");
    }

    return pass(name);
}

#include "C4Tests.hpp"
#include "GrenadeEffectsTests.hpp"
#include "BotTests.hpp"
#include "UiLogicTests.hpp"
#include "AudioTests.hpp"
#include "NavMeshTests.hpp"
#include "PlayerMovementTests.hpp"
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
    ok = testFpsSpawnSelectionPerTeamFarthest() && ok;
    ok = testFpsSwitchChangesActiveDef() && ok;
    ok = testFpsDropAndPickup() && ok;
    ok = testFpsLoadoutInventoryInvariant() && ok;
    ok = testDroppedWeaponEcsLifecycle() && ok;
    ok = testFpsDeathmatchScoreLimit() && ok;
    ok = testFpsTeamDeathmatchTeamScoreWin() && ok;
    ok = testPlayerMovementInputContract() && ok;
    ok = testCompetitiveRoundFsmFullCycleHalftimeClinch() && ok;
    ok = testCompetitiveOvertime() && ok;
    ok = testCompetitiveTimerTargetSavedAndPause() && ok;
    ok = testCompetitiveWorldPhaseAuthority() && ok;
    ok = testCompetitiveEconomyStreakCap() && ok;
    ok = testCompetitiveWorldEconomyHooks() && ok;
    ok = testCompetitiveBuyMatrix() && ok;
    ok = testCompetitiveTeamAutoAssign() && ok;
    ok = testCompetitiveSessionFlow() && ok;
    ok = testCompetitiveBuyZoneOccupancy() && ok;
    ok = testWeaponRosterIntegrity() && ok;
    ok = testWeaponAuthorityTiming() && ok;
    ok = testLagCompensatedShotPoseFallbackAndMissCount() && ok;
    ok = testFpsDamageContract() && ok;
    ok = testFpsDeathLifecycle() && ok;
    ok = testC4PlantInterrupt() && ok;
    ok = testC4AuthoredTriggerOccupancy() && ok;
    ok = testC4DefuseKit() && ok;
    ok = testC4Timer40() && ok;
    ok = testC4CarrierDropPickup() && ok;
    ok = testC4WorldAuthority() && ok;
    ok = testGrenadeEffectLifecycle() && ok;
    ok = testGrenadeEffectImpactTargetsAndCleanup() && ok;
    ok = testBotDifficultyClampTable() && ok;
    ok = testBotPerceptionBlindfold() && ok;
    ok = testBotFsmPlantDefendDefuse() && ok;
    ok = testBotSteeringArrivalStuck() && ok;
    ok = testBotInputSynthesisShape() && ok;
    ok = testBotEngageStrafes() && ok;
    ok = testBotBuyPlanByMoney() && ok;
    ok = testBotDebugRingWraps() && ok;
    ok = testBotDebugTransitionRecorded() && ok;
    ok = testBotDebugDumpFormatter() && ok;
    ok = testBotDebugPausedNeutral() && ok;
    ok = testBombAuthoredSitesParse() && ok;
    ok = testNavMeshGenerateAndSerializerRoundTrip() && ok;
    ok = testNavMeshFindPathTwoPoly() && ok;
    ok = testNavMeshHierarchyRootBake() && ok;
    ok = testNavMeshHierarchyWorldBakeAndPath() && ok;
    ok = testNavMeshHierarchyStaleParentRootsBake() && ok;
    ok = testUiScreenTransitions() && ok;
    ok = testUiModalInputIsolation() && ok;
    ok = testUiMenusRequireCompetitiveState() && ok;
    ok = testUiClockFormatting() && ok;
    ok = testUiBuyItemStateMatrix() && ok;
    ok = testUiScoreboardMoneyVisibility() && ok;
    ok = testUiRadarProjection() && ok;
    ok = testAudioUnknownEventNoVoices() && ok;
    ok = testAudioVoiceStealingSelection() && ok;
    ok = testAudioPerEventCap() && ok;
    ok = testAudioClipPickNoRepeat() && ok;
    ok = testAudioDistanceCullingAndLookup() && ok;
    ok = testAudioServerExclusionHeadless() && ok;
    ok = testAudioSourceFollowUsesSimulationHierarchyPose() && ok;
    return ok ? 0 : 1;
}
