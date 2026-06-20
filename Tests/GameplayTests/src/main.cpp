#include "Components/Components.hpp"
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
    ok = testGameModeSpawnsAtPlayerStart() && ok;
    ok = testLevelMetadataAppliesGameplaySettings() && ok;
    return ok ? 0 : 1;
}
