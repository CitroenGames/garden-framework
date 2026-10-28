#include "world.hpp"

#include "GameFramework/GameFrameworkRuntime.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"

#include <utility>

world::world(const PhysicsSystemSettings& physics_settings)
    : simulation_ticks(physics_settings.fixed_delta)
{
    gameplay_framework = std::make_unique<GameFramework::GameFrameworkRuntime>();
    fixed_delta = physics_settings.fixed_delta;
    physics_system = std::make_unique<PhysicsSystem>(physics_settings);
    fixed_delta = physics_system->getFixedDelta();
    simulation_ticks.setFixedDelta(fixed_delta);
}

world::world(world&& other) noexcept
    : physics_system(std::move(other.physics_system))
    , gameplay_framework(std::move(other.gameplay_framework))
    , simulation_ticks(std::move(other.simulation_ticks))
    , simulation_tick(other.simulation_tick)
    , simulation_time(other.simulation_time)
    , registry(std::move(other.registry))
    , world_camera(std::move(other.world_camera))
    , fixed_delta(other.fixed_delta)
{
    other.registry = entt::registry{};
    if (gameplay_framework)
        gameplay_framework->rebindMovedWorld(other, *this);
}

world& world::operator=(world&& other) noexcept
{
    if (this == &other)
        return *this;
    shutdown();
    physics_system = std::move(other.physics_system);
    gameplay_framework = std::move(other.gameplay_framework);
    simulation_ticks = std::move(other.simulation_ticks);
    simulation_tick = other.simulation_tick;
    simulation_time = other.simulation_time;
    registry = std::move(other.registry);
    other.registry = entt::registry{};
    world_camera = std::move(other.world_camera);
    fixed_delta = other.fixed_delta;
    if (gameplay_framework)
        gameplay_framework->rebindMovedWorld(other, *this);
    return *this;
}

world::~world()
{
    shutdown();
}

void world::setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> mode)
{
    if (!gameplay_framework)
        gameplay_framework = std::make_unique<GameFramework::GameFrameworkRuntime>();
    gameplay_framework->setAuthorityGameMode(*this, std::move(mode));
}

GameFramework::GameModeBase* world::getAuthorityGameMode() { return gameplay_framework ? gameplay_framework->getAuthorityGameMode() : nullptr; }
const GameFramework::GameModeBase* world::getAuthorityGameMode() const { return gameplay_framework ? gameplay_framework->getAuthorityGameMode() : nullptr; }
void world::setGameState(std::unique_ptr<GameFramework::GameStateBase> state)
{
    if (!gameplay_framework)
        gameplay_framework = std::make_unique<GameFramework::GameFrameworkRuntime>();
    gameplay_framework->setGameState(std::move(state));
}
GameFramework::GameStateBase* world::getGameState() { return gameplay_framework ? gameplay_framework->getGameState() : nullptr; }
const GameFramework::GameStateBase* world::getGameState() const { return gameplay_framework ? gameplay_framework->getGameState() : nullptr; }
void world::clearGameplayFramework() { if (gameplay_framework) gameplay_framework->clear(); }
bool world::initializeGameplayFramework(const std::string& map_name, const std::string& options) { return gameplay_framework && gameplay_framework->initialize(*this, map_name, options); }
void world::startGameplayFramework() { if (gameplay_framework) gameplay_framework->start(*this); }
void world::tickGameplayFramework(float delta_time) { if (gameplay_framework) gameplay_framework->tick(*this, delta_time); }
void world::shutdownGameplayFramework() { if (gameplay_framework) gameplay_framework->shutdown(); }
bool world::isGameplayFrameworkInitialized() const { return gameplay_framework && gameplay_framework->isInitialized(); }
bool world::hasGameplayFrameworkStarted() const { return gameplay_framework && gameplay_framework->hasStarted(); }

void world::shutdown()
{
    clearGameplayFramework();

    if (physics_system)
        physics_system->shutdown();
    clearRegistryStorage();
    simulation_ticks.reset();
    simulation_tick = 0;
    simulation_time = 0.0;
}

void world::resetWorld()
{
    shutdown();
    if (physics_system)
        physics_system->initialize();
}

entt::entity world::resolveEntity(EntityRef entity) const
{
    return resolveEntityRef(registry, entity);
}

bool world::isEntityValid(EntityRef entity) const
{
    return resolveEntity(entity) != entt::null;
}

EntityRef world::createEntity()
{
    const entt::entity entity = registry.create();
    return ensureEntityRef(registry, entity);
}

EntityRef world::findEntityByTag(const std::string& name) const
{
    if (name.empty())
        return {};
    entt::entity match = entt::null;
    auto tagged = registry.view<TagComponent>();
    for (const entt::entity entity : tagged) {
        if (tagged.get<TagComponent>(entity).name != name)
            continue;
        if (match != entt::null)
            return {};
        match = entity;
    }
    return entityRefOf(registry, match);
}
