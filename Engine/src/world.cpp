#include "world.hpp"

#include "GameFramework/GameFrameworkRuntime.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "ECS/EngineComponentAdapters.hpp"

#include <atomic>
#include <exception>
#include <utility>

namespace {
std::atomic<uint64_t> nextComponentWorldToken{1};
struct ScopedEntityMarker {
    garden::ecs::ComponentStore* store;
    garden::ecs::ComponentEntityKey key;
    uint64_t serial;
};
void retireScopedEntity(entt::registry& registry, entt::entity entity)
{
    const auto& marker = registry.get<ScopedEntityMarker>(entity);
    (void)marker.store->removeEntity(marker.key);
}
}

struct world::ComponentWorldState {
    garden::ecs::ComponentStore store;
    uint64_t token = nextComponentWorldToken.fetch_add(1, std::memory_order_relaxed);
    uint64_t nextSerial = 1;
};

world::world(const PhysicsSystemSettings& physics_settings)
    : simulation_ticks(physics_settings.fixed_delta)
{
    garden::ecs::engine_adapters::registerComponents();
    fixed_delta = physics_settings.fixed_delta;
    physics_system = std::make_unique<PhysicsSystem>(physics_settings);
    fixed_delta = physics_system->getFixedDelta();
    simulation_ticks.setFixedDelta(fixed_delta);
}

world::world(world&& other) noexcept
    : component_state([&other] {
        // Moving the source would invalidate an active callback's world.
        if (other.active_queries) std::terminate();
        return std::move(other.component_state);
    }())
    , physics_system(std::move(other.physics_system))
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
    if (this == &other || active_queries || other.active_queries)
        return *this;
    shutdown();
    component_state = std::move(other.component_state);
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
    if (active_queries) return;
    clearGameplayFramework();

    if (physics_system)
        physics_system->shutdown();
    clearRegistryStorage();
    component_state.reset();
    simulation_ticks.reset();
    simulation_tick = 0;
    simulation_time = 0.0;
}

void world::resetWorld()
{
    if (active_queries) return;
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
    if (active_queries) return {};
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

garden::ecs::ComponentStore& world::componentStore() noexcept { return component_state->store; }
const garden::ecs::ComponentStore& world::componentStore() const noexcept { return component_state->store; }

garden::ecs::ComponentStoreResult world::validateBackend(garden::ecs::ComponentKey key, garden::ecs::ComponentId id) noexcept
{ return garden::ecs::engine_adapters::validate(key, id); }
bool world::isAdapted(garden::ecs::ComponentId id) const noexcept
{ return garden::ecs::engine_adapters::handles(id); }
garden::ecs::ComponentStoreResult world::addAdapted(garden::ecs::ComponentId id, garden::ecs::ComponentEntityKey key, bool (*construct)(void*, void*) noexcept, void* context) noexcept
{ return garden::ecs::engine_adapters::add(registry, id, key, construct, context); }
garden::ecs::ComponentStoreResult world::getValue(garden::ecs::ComponentId id, garden::ecs::ComponentEntityKey key, void*& out) const noexcept
{ return isAdapted(id) ? garden::ecs::engine_adapters::get(registry, id, key, out) : componentStore().get(id, key, out); }
garden::ecs::ComponentStoreResult world::removeValue(garden::ecs::ComponentId id, garden::ecs::ComponentEntityKey key) noexcept
{ return isAdapted(id) ? garden::ecs::engine_adapters::remove(registry, id, key) : componentStore().remove(id, key); }
garden::ecs::ComponentStoreResult world::snapshotValues(garden::ecs::ComponentId id, std::vector<garden::ecs::ComponentEntityKey>& out) const noexcept
{ return isAdapted(id) ? garden::ecs::engine_adapters::snapshot(registry, id, out) : componentStore().snapshotEntities(id, out); }

garden::ecs::ComponentStoreResult world::scopedKey(garden::ecs::EntityHandle handle,
    garden::ecs::ComponentEntityKey& key) const noexcept
{
    using namespace garden::ecs;
    if (!component_state || !handle.m_world || handle.m_world != component_state->token ||
        handle.m_entity == UINT32_MAX) return ComponentStoreResult::InvalidEntity;
    const auto entity = static_cast<entt::entity>(handle.m_entity);
    if (!registry.valid(entity) || !registry.all_of<ScopedEntityMarker>(entity))
        return ComponentStoreResult::StaleEntity;
    const auto& marker = registry.get<ScopedEntityMarker>(entity);
    if (!handle.m_serial || marker.serial != handle.m_serial) return ComponentStoreResult::StaleEntity;
    key = marker.key;
    return ComponentStoreResult::Success;
}

bool world::isEntityValid(garden::ecs::EntityHandle handle) const noexcept
{
    garden::ecs::ComponentEntityKey key;
    return scopedKey(handle, key) == garden::ecs::ComponentStoreResult::Success;
}

garden::ecs::ComponentStoreResult world::snapshotIdentityCounts(IdentityCounts& counts) const noexcept
{
    try {
        for (const auto [entity, identity] : registry.view<IdentityComponent>().each()) {
            (void)entity;
            if (!identity.guid.isNull()) ++counts[identity.guid];
        }
        return garden::ecs::ComponentStoreResult::Success;
    } catch (...) {
        return garden::ecs::ComponentStoreResult::ResourceExhausted;
    }
}

garden::ecs::EntityHandle world::handleForKey(garden::ecs::ComponentEntityKey key,
    const IdentityCounts& counts) const noexcept
{
    if (!component_state || !key.generation) return {};
    const auto entity = entt::entt_traits<entt::entity>::construct(key.slot, key.generation - 1u);
    if (!registry.valid(entity) || !registry.all_of<ScopedEntityMarker>(entity)) return {};
    const auto& marker = registry.get<ScopedEntityMarker>(entity);
    if (marker.key.slot != key.slot || marker.key.generation != key.generation) return {};
    const auto* identity = registry.try_get<IdentityComponent>(entity);
    if (!identity || identity->guid.isNull()) return {};
    const auto count = counts.find(identity->guid);
    if (count == counts.end() || count->second != 1) return {};
    garden::ecs::EntityHandle handle;
    handle.m_world = component_state->token;
    handle.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
    handle.m_serial = marker.serial;
    return handle;
}

garden::ecs::EntityHandle world::scopedEntity(EntityRef reference)
{
    if (active_queries) return {};
    if (reference.isNull()) return {};
    entt::entity entity = entt::null;
    for (auto [candidate, identity] : registry.view<IdentityComponent>().each()) {
        if (identity.guid != reference.guid) continue;
        if (entity != entt::null) return {};
        entity = candidate;
    }
    if (entity == entt::null) return {};
    if (!component_state) component_state = std::make_unique<ComponentWorldState>();
    // Registry reset drops component storage and its signal. Reconnect exactly
    // once whenever a new scoped entity is attached, including after a move.
    auto sink = registry.on_destroy<ScopedEntityMarker>();
    sink.disconnect<&retireScopedEntity>();
    sink.connect<&retireScopedEntity>();
    if (!registry.all_of<ScopedEntityMarker>(entity)) {
        const garden::ecs::ComponentEntityKey key{
            static_cast<uint32_t>(entt::to_entity(entity)),
            static_cast<uint32_t>(entt::to_version(entity)) + 1u};
        // Refuse reuse if the per-world serial space is exhausted.
        if (!component_state->nextSerial) return {};
        registry.emplace<ScopedEntityMarker>(entity, &component_state->store, key, component_state->nextSerial++);
    }
    garden::ecs::EntityHandle result;
    result.m_world = component_state->token;
    result.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
    result.m_serial = registry.get<ScopedEntityMarker>(entity).serial;
    return result;
}

garden::ecs::EntityHandle world::createScopedEntity()
{
    if (active_queries) return {};
    EntityRef reference;
    do {
        reference.guid = generateEntityGuid();
    } while (resolveEntityRef(registry, reference) != entt::null);
    return restoreScopedEntity(reference);
}

garden::ecs::EntityHandle world::restoreScopedEntity(EntityRef reference)
{
    if (active_queries || reference.isNull() || resolveEntityRef(registry, reference) != entt::null)
        return {};

    entt::entity entity = entt::null;
    try {
        if (!component_state) component_state = std::make_unique<ComponentWorldState>();
        if (!component_state->nextSerial) return {};
        auto sink = registry.on_destroy<ScopedEntityMarker>();
        sink.disconnect<&retireScopedEntity>();
        sink.connect<&retireScopedEntity>();
        entity = registry.create();
        const garden::ecs::ComponentEntityKey key{
            static_cast<uint32_t>(entt::to_entity(entity)),
            static_cast<uint32_t>(entt::to_version(entity)) + 1u};
        const uint64_t serial = component_state->nextSerial++;
        registry.emplace<ScopedEntityMarker>(entity, &component_state->store, key, serial);
        // Publish the final identity only after scope exists, so identity hooks
        // observe a fully scoped entity and index the supplied GUID directly.
        registry.emplace<IdentityComponent>(entity, IdentityComponent{reference.guid});
        garden::ecs::EntityHandle result;
        result.m_world = component_state->token;
        result.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
        result.m_serial = serial;
        return result;
    } catch (...) {
        if (entity != entt::null && registry.valid(entity)) registry.destroy(entity);
        return {};
    }
}

EntityRef world::persistedReference(garden::ecs::EntityHandle handle) const
{
    garden::ecs::ComponentEntityKey key;
    if (scopedKey(handle, key) != garden::ecs::ComponentStoreResult::Success) return {};
    const auto entity = static_cast<entt::entity>(handle.m_entity);
    const auto* identity = registry.try_get<IdentityComponent>(entity);
    if (!identity || identity->guid.isNull()) return {};
    const EntityRef reference{identity->guid};
    if (query_identity_counts) {
        const auto count = query_identity_counts->find(reference.guid);
        return count != query_identity_counts->end() && count->second == 1 ? reference : EntityRef{};
    }
    for (const auto [candidate, other] : registry.view<IdentityComponent>().each()) {
        if (candidate != entity && other.guid == reference.guid) return {};
    }
    return reference;
}

garden::ecs::ComponentStoreResult world::destroyEntity(garden::ecs::EntityHandle handle) noexcept
{
    if (active_queries) return garden::ecs::ComponentStoreResult::StructuralMutationDuringQuery;
    garden::ecs::ComponentEntityKey key;
    const auto result = scopedKey(handle, key);
    if (result != garden::ecs::ComponentStoreResult::Success) return result;
    try {
        registry.destroy(static_cast<entt::entity>(handle.m_entity));
        return garden::ecs::ComponentStoreResult::Success;
    } catch (...) {
        return garden::ecs::ComponentStoreResult::InternalError;
    }
}
