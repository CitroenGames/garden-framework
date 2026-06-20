#include "world.hpp"

#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "Utils/Log.hpp"

world::world(const PhysicsSystemSettings& physics_settings)
    : simulation_ticks(physics_settings.fixed_delta)
{
    world_camera = camera(0, 0, -5);
    fixed_delta = physics_settings.fixed_delta;
    physics_system = std::make_unique<PhysicsSystem>(physics_settings);
    fixed_delta = physics_system->getFixedDelta();
    simulation_ticks.setFixedDelta(fixed_delta);
}

world::world(world&&) noexcept = default;

world& world::operator=(world&&) noexcept = default;

world::~world()
{
    shutdown();
}

void world::setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> game_mode)
{
    authority_game_mode = std::move(game_mode);
    if (authority_game_mode)
        authority_game_mode->bindWorld(*this);
}

void world::setGameState(std::unique_ptr<GameFramework::GameStateBase> state)
{
    game_state = std::move(state);
}

void world::clearGameplayFramework()
{
    shutdownGameplayFramework();
    authority_game_mode.reset();
    game_state.reset();
}

bool world::initializeGameplayFramework(const std::string& map_name, const std::string& options)
{
    if (gameplay_framework_initialized)
        return true;

    if (authority_game_mode)
    {
        std::string error_message;
        authority_game_mode->initGame(*this, map_name, options, error_message);
        if (!error_message.empty())
        {
            LOG_ENGINE_WARN("GameMode init reported: {}", error_message);
            return false;
        }

        gameplay_framework_initialized = true;
        return true;
    }

    if (game_state)
    {
        game_state->initialize(this);
        game_state->setAuthorityGameMode(nullptr);
        gameplay_framework_initialized = true;
        return true;
    }

    return false;
}

void world::startGameplayFramework()
{
    if (!gameplay_framework_initialized && !initializeGameplayFramework())
        return;
    if (gameplay_framework_started)
        return;

    if (authority_game_mode)
        authority_game_mode->startPlay();
    else if (game_state)
        game_state->handleBeginPlay();

    gameplay_framework_started = true;
}

void world::tickGameplayFramework(float delta_time)
{
    if (!gameplay_framework_initialized && !initializeGameplayFramework())
        return;

    if (authority_game_mode)
        authority_game_mode->tick(delta_time);
    else if (game_state)
        game_state->tick(delta_time);
}

void world::shutdownGameplayFramework()
{
    if (game_state && gameplay_framework_initialized)
        game_state->shutdown();

    gameplay_framework_initialized = false;
    gameplay_framework_started = false;
}

void world::shutdown()
{
    clearGameplayFramework();

    if (physics_system)
        physics_system->shutdown();
    clearRegistryStorage();
    simulation_ticks.reset();
    simulation_tick = 0;
}

void world::resetWorld()
{
    shutdown();
    if (physics_system)
        physics_system->initialize();
}
