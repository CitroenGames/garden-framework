#include "world.hpp"

#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"

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
    authority_game_mode.reset();
    game_state.reset();
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
