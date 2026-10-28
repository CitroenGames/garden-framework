#include "world.hpp"

#include "GameFramework/GameFrameworkRuntime.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "ECS/EngineComponentAdapters.hpp"
#include "ECS/RetirementFence.hpp"
#include "ECS/Hierarchy.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include "Utils/Log.hpp"
#include <list>
#include <algorithm>

#include <atomic>
#include <exception>
#include <unordered_set>
#include <utility>

namespace {
std::atomic<uint64_t> nextComponentWorldToken{1};
struct ScopedEntityMarker {
    garden::ecs::ComponentStore* store;
    garden::ecs::ComponentEntityKey key;
    uint64_t serial;
};
bool hasLiveEntities(entt::registry& registry)
{
    const auto entities = registry.storage<entt::entity>().each();
    return entities.begin() != entities.end();
}
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

struct world::LifecycleState {
    struct Member {
        entt::entity entity = entt::null;
        garden::ecs::EntityHandle handle{};
        LifecyclePhase phase = LifecyclePhase::Event;
        bool event_attempted = false;
    };
    struct Request { garden::ecs::EntityHandle handle; bool unpublished = false; };
    std::list<Request> queue;
    std::vector<garden::ecs::EntityHandle> rollback;
    std::vector<Member> cohort;
    size_t current = 0;
    size_t completed = 0;
    bool running = false;
    bool constructing = false;
    bool unpublished = false;
    bool constraints_ready = false;
    bool observer_failure = false;
    bool shutdown_observer_failure = false;
    bool shutdown_complete = false;
    bool shutting_down = false;
};

world::world(const PhysicsSystemSettings& physics_settings)
    : simulation_ticks(physics_settings.fixed_delta)
{
    lifecycle_state = std::make_unique<LifecycleState>();
    retirement_fence = std::make_unique<RetirementFence>();
    retirement_fence->rollback_context = lifecycle_state.get();
    retirement_fence->rollback_contains = [](const void* context, entt::entity entity) noexcept {
        const auto& rollback = static_cast<const LifecycleState*>(context)->rollback;
        return std::any_of(rollback.begin(), rollback.end(), [entity](const auto& handle) {
            return handle.m_entity == static_cast<uint32_t>(entt::to_integral(entity));
        });
    };
    registry.ctx().emplace<RetirementFenceBinding>(retirement_fence.get());
    garden::ecs::engine_adapters::registerComponents();
    fixed_delta = physics_settings.fixed_delta;
    physics_system = std::make_unique<PhysicsSystem>(physics_settings);
    physics_system->bindRetirementFence(retirement_fence.get());
    fixed_delta = physics_system->getFixedDelta();
    simulation_ticks.setFixedDelta(fixed_delta);
}

world::~world()
{
    const auto result = shutdown();
    if (!result.cleanupComplete()) {
        try { LOG_ENGINE_ERROR("World destructor: incomplete retirement, phase {}", static_cast<int>(result.phase)); }
        catch (...) {}
    }
}

void world::markWorldActive() noexcept { lifecycle_state->shutdown_complete = false; }

void world::setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> mode)
{
    lifecycle_state->shutdown_complete = false;
    if (!gameplay_framework)
        gameplay_framework = std::make_unique<GameFramework::GameFrameworkRuntime>();
    gameplay_framework->setAuthorityGameMode(*this, std::move(mode));
}

GameFramework::GameModeBase* world::getAuthorityGameMode() { return gameplay_framework ? gameplay_framework->getAuthorityGameMode() : nullptr; }
const GameFramework::GameModeBase* world::getAuthorityGameMode() const { return gameplay_framework ? gameplay_framework->getAuthorityGameMode() : nullptr; }
void world::setGameState(std::unique_ptr<GameFramework::GameStateBase> state)
{
    lifecycle_state->shutdown_complete = false;
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

LifecycleResult world::shutdown() noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || lifecycle_state->constructing) return {LifecycleStatus::Reentrant};
    if (lifecycle_state->shutdown_complete && !hasLiveEntities(registry))
        return {LifecycleStatus::Completed};
    lifecycle_state->shutting_down = true;
    auto drained = drainRetirements();
    if (!drained.cleanupComplete()) {
        if (drained.status == LifecycleStatus::ObserverFailure) lifecycle_state->shutdown_observer_failure = true;
        return drained;
    }
    size_t completed = drained.completed;
    if (drained.status == LifecycleStatus::CompletedWithObserverFailure)
        lifecycle_state->shutdown_observer_failure = true;
    try {
        std::vector<entt::entity> entities;
        for (const auto [entity] : registry.storage<entt::entity>().each()) entities.push_back(entity);
        for (const auto entity : entities) {
            if (!registry.valid(entity)) continue;
            auto result = retireRaw(entity, false);
            result.completed += completed;
            if (!result.cleanupComplete()) {
                if (result.status == LifecycleStatus::ObserverFailure) lifecycle_state->shutdown_observer_failure = true;
                return result;
            }
            completed = result.completed;
            if (result.status == LifecycleStatus::CompletedWithObserverFailure)
                lifecycle_state->shutdown_observer_failure = true;
        }
        if (hasLiveEntities(registry))
            return {LifecycleStatus::Partial, LifecyclePhase::Identity, {}, completed};
        clearGameplayFramework();
        if (physics_system) physics_system->shutdown();
        clearRegistryStorage();
        component_state.reset();
        lifecycle_state->queue.clear();
        retirement_fence->pending.clear();
        simulation_ticks.reset();
        simulation_tick = 0;
        simulation_time = 0.0;
        const auto status = lifecycle_state->shutdown_observer_failure
            ? LifecycleStatus::CompletedWithObserverFailure : LifecycleStatus::Completed;
        lifecycle_state->shutdown_observer_failure = false;
        lifecycle_state->shutdown_complete = true;
        lifecycle_state->shutting_down = false;
        return {status, LifecyclePhase::None, {}, completed};
    } catch (const std::bad_alloc&) {
        return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare, {}, completed};
    } catch (...) {
        return {LifecycleStatus::Partial, LifecyclePhase::Identity, {}, completed};
    }
}

LifecycleResult world::resetWorld(const PhysicsSystemSettings& settings) noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || lifecycle_state->constructing) return {LifecycleStatus::Reentrant};
    try {
        // Prepare every replacement resource before retiring the old world.
        auto replacement_components = std::make_unique<ComponentWorldState>();
        auto replacement_physics = std::make_unique<PhysicsSystem>(settings);
        replacement_physics->bindRetirementFence(retirement_fence.get());
        replacement_physics->initialize();
        camera replacement_camera{};
        auto result = shutdown();
        if (!result.cleanupComplete()) return result;
        component_state = std::move(replacement_components);
        physics_system = std::move(replacement_physics);
        fixed_delta = physics_system->getFixedDelta();
        simulation_ticks.setFixedDelta(fixed_delta);
        world_camera = std::move(replacement_camera);
        lifecycle_state->shutdown_complete = false;
        return result;
    } catch (const std::bad_alloc&) {
        return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare};
    } catch (...) {
        return {LifecycleStatus::InternalFailure, LifecyclePhase::Prepare};
    }
}

entt::entity world::resolveEntity(EntityRef entity) const
{
    return resolveEntityRef(registry, entity);
}

bool world::isEntityValid(EntityRef entity) const
{
    return resolveEntity(entity) != entt::null;
}

EntityRef world::createEntity(std::string_view tag)
{
    const auto handle = createScopedEntity(tag);
    return persistedReference(handle);
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
    if (!component_state || handle.m_owner != reinterpret_cast<uintptr_t>(this) || !handle.m_world || handle.m_world != component_state->token ||
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
    handle.m_owner = reinterpret_cast<uintptr_t>(this);
    handle.m_world = component_state->token;
    handle.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
    handle.m_serial = marker.serial;
    return handle;
}

garden::ecs::EntityHandle world::scopedEntity(EntityRef reference)
{
    if (active_queries) return {};
    if (reference.isNull()) return {};
    lifecycle_state->shutdown_complete = false;
    entt::entity entity = entt::null;
    for (auto [candidate, identity] : registry.view<IdentityComponent>().each()) {
        if (identity.guid != reference.guid) continue;
        if (entity != entt::null) return {};
        entity = candidate;
    }
    if (entity == entt::null) return {};
    if (!component_state) component_state = std::make_unique<ComponentWorldState>();
    // Registry reset drops component storage and its signal. Reconnect exactly
    // once whenever a new scoped entity is attached, including after reset.
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
    result.m_owner = reinterpret_cast<uintptr_t>(this);
    result.m_world = component_state->token;
    result.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
    result.m_serial = registry.get<ScopedEntityMarker>(entity).serial;
    return result;
}

garden::ecs::EntityHandle world::createScopedEntity(std::string_view tag)
{
    if (active_queries || lifecycle_state->shutting_down || lifecycle_state->running ||
        !lifecycle_state->cohort.empty() || !lifecycle_state->rollback.empty()) return {};
    EntityRef reference;
    do {
        reference.guid = generateEntityGuid();
    } while (resolveEntityRef(registry, reference) != entt::null);
    auto handle = restoreScopedEntity(reference);
    if (!isEntityValid(handle)) return {};
    const auto entity = static_cast<entt::entity>(handle.m_entity);
    if (!tag.empty()) registry.emplace<TagComponent>(entity, TagComponent{std::string(tag)});
    EventBus::get().queue(EntityCreatedEvent{reference, std::string(tag)});
    return handle;
}

garden::ecs::EntityHandle world::restoreScopedEntity(EntityRef reference)
{
    std::vector<garden::ecs::EntityHandle> handles;
    if (!restoreScopedEntities({reference}, handles)) return {};
    return handles.front();
}

bool world::restoreScopedEntities(const std::vector<EntityRef>& references,
                                  std::vector<garden::ecs::EntityHandle>& out_handles)
{
    if (active_queries || lifecycle_state->shutting_down || lifecycle_state->running || lifecycle_state->constructing ||
        !lifecycle_state->cohort.empty() || !lifecycle_state->rollback.empty()) return false;
    lifecycle_state->shutdown_complete = false;
    std::vector<entt::entity> entities;
    std::vector<garden::ecs::EntityHandle> handles;
    try {
        std::unordered_set<EntityGuid, GuidHash> requested;
        requested.reserve(references.size());
        for (const auto& ref : references)
            if (ref.isNull() || !requested.insert(ref.guid).second) return false;
        for (const auto [entity, identity] : registry.view<IdentityComponent>().each()) {
            (void)entity;
            if (requested.count(identity.guid)) return false;
        }
        entities.reserve(references.size());
        handles.reserve(references.size());
        if (!component_state) component_state = std::make_unique<ComponentWorldState>();
        auto sink = registry.on_destroy<ScopedEntityMarker>();
        sink.disconnect<&retireScopedEntity>();
        sink.connect<&retireScopedEntity>();
        lifecycle_state->constructing = true;
        for (const auto& ref : references) {
            if (!component_state->nextSerial) throw 0;
            const entt::entity entity = registry.create();
            entities.push_back(entity);
            const garden::ecs::ComponentEntityKey key{
                static_cast<uint32_t>(entt::to_entity(entity)),
                static_cast<uint32_t>(entt::to_version(entity)) + 1u};
            const uint64_t serial = component_state->nextSerial++;
            garden::ecs::EntityHandle handle;
            handle.m_owner = reinterpret_cast<uintptr_t>(this);
            handle.m_world = component_state->token;
            handle.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
            handle.m_serial = serial;
            // Ownership must precede construction hooks, including marker hooks.
            handles.push_back(handle);
            registry.emplace<ScopedEntityMarker>(entity, &component_state->store, key, serial);
            registry.emplace<IdentityComponent>(entity, IdentityComponent{ref.guid});
            if (!registry.valid(entity) || !registry.all_of<ScopedEntityMarker, IdentityComponent>(entity) ||
                registry.get<IdentityComponent>(entity).guid != ref.guid)
                throw 0;
        }
        // Hooks may have modified identities or created colliding entities.
        std::unordered_map<EntityGuid, size_t, GuidHash> counts;
        counts.reserve(references.size());
        for (const auto [entity, identity] : registry.view<IdentityComponent>().each()) {
            (void)entity;
            if (requested.count(identity.guid)) ++counts[identity.guid];
        }
        for (size_t i = 0; i < references.size(); ++i)
            if (!registry.valid(entities[i]) ||
                !registry.all_of<ScopedEntityMarker, IdentityComponent>(entities[i]) ||
                registry.get<ScopedEntityMarker>(entities[i]).serial != handles[i].m_serial ||
                registry.get<ScopedEntityMarker>(entities[i]).store != &component_state->store ||
                registry.get<ScopedEntityMarker>(entities[i]).key.slot != static_cast<uint32_t>(entt::to_entity(entities[i])) ||
                registry.get<ScopedEntityMarker>(entities[i]).key.generation != static_cast<uint32_t>(entt::to_version(entities[i])) + 1u ||
                registry.get<IdentityComponent>(entities[i]).guid != references[i].guid ||
                counts[references[i].guid] != 1) throw 0;
        lifecycle_state->constructing = false;
        out_handles = std::move(handles);
        return true;
    } catch (...) {
        lifecycle_state->constructing = false;
        ConstructionTransaction transaction{this, std::move(handles)};
        (void)abortUnpublished(transaction);
        return false;
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

LifecycleStatus world::validateLifecycleHandle(garden::ecs::EntityHandle handle) const noexcept
{
    if (!handle.m_owner || !handle.m_world || handle.m_entity == UINT32_MAX || !handle.m_serial)
        return LifecycleStatus::Invalid;
    if (handle.m_owner != reinterpret_cast<uintptr_t>(this)) return LifecycleStatus::WrongWorld;
    if (!component_state || handle.m_world != component_state->token) return LifecycleStatus::Stale;
    garden::ecs::ComponentEntityKey key;
    return scopedKey(handle, key) == garden::ecs::ComponentStoreResult::Success
        ? LifecycleStatus::Alive : LifecycleStatus::Stale;
}

LifecycleResult world::retirementState(garden::ecs::EntityHandle handle) const noexcept
{
    // An unpublished entity can lose its marker in a construct hook yet still
    // belong to the rollback transaction and remain fenced until retired.
    if (std::find(lifecycle_state->rollback.begin(), lifecycle_state->rollback.end(), handle) != lifecycle_state->rollback.end() &&
        registry.valid(static_cast<entt::entity>(handle.m_entity)))
        return {LifecycleStatus::Pending, LifecyclePhase::None, handle};
    const auto valid = validateLifecycleHandle(handle);
    if (valid != LifecycleStatus::Alive) return {valid, LifecyclePhase::None, handle};
    if (retirement_fence->blocks(static_cast<entt::entity>(handle.m_entity)) ||
        std::find(lifecycle_state->rollback.begin(), lifecycle_state->rollback.end(), handle) != lifecycle_state->rollback.end() ||
        std::find_if(lifecycle_state->queue.begin(), lifecycle_state->queue.end(),
            [&](const auto& request) { return request.handle == handle; }) != lifecycle_state->queue.end())
        return {LifecycleStatus::Pending, LifecyclePhase::None, handle};
    return {LifecycleStatus::Alive, LifecyclePhase::None, handle};
}

LifecycleResult world::requestRetirement(garden::ecs::EntityHandle handle) noexcept
{
    if (lifecycle_state->constructing) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    const auto state = retirementState(handle);
    if (state.status == LifecycleStatus::Pending) return {LifecycleStatus::AlreadyPending, LifecyclePhase::None, handle};
    if (state.status != LifecycleStatus::Alive) return state;
    try {
        const auto raw = static_cast<entt::entity>(handle.m_entity);
        retirement_fence->pending.insert(raw);
        try { lifecycle_state->queue.push_back({handle, false}); }
        catch (...) { retirement_fence->pending.erase(raw); throw; }
        return {LifecycleStatus::Queued, LifecyclePhase::None, handle};
    } catch (...) {
        return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare, handle};
    }
}

LifecycleResult world::retireEntity(garden::ecs::EntityHandle handle) noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive, LifecyclePhase::None, handle};
    if (lifecycle_state->running) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    const auto status = validateLifecycleHandle(handle);
    if (status != LifecycleStatus::Alive) return {status, LifecyclePhase::None, handle};
    if (!lifecycle_state->cohort.empty()) {
        if (!retirement_fence->contains(static_cast<entt::entity>(handle.m_entity)))
            return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
        return processRetirement();
    }
    return retireRaw(static_cast<entt::entity>(handle.m_entity), false);
}

LifecycleResult world::retireRaw(entt::entity entity, bool unpublished) noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || !lifecycle_state->cohort.empty()) return {LifecycleStatus::Reentrant};
    if (entity == entt::null || !registry.valid(entity)) return {LifecycleStatus::Stale};
    garden::ecs::EntityHandle current;
    if (component_state) if (const auto* marker = registry.try_get<ScopedEntityMarker>(entity)) {
        current.m_owner = reinterpret_cast<uintptr_t>(this);
        current.m_world = component_state->token;
        current.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
        current.m_serial = marker->serial;
    }
    try {
        std::vector<entt::entity> entities;
        if (unpublished && !lifecycle_state->rollback.empty()) {
            entities.reserve(lifecycle_state->rollback.size());
            for (auto it = lifecycle_state->rollback.rbegin(); it != lifecycle_state->rollback.rend(); ++it) {
                if (registry.valid(static_cast<entt::entity>(it->m_entity)))
                    entities.push_back(static_cast<entt::entity>(it->m_entity));
            }
        } else entities.push_back(entity);
        const auto* policy = registry.try_get<HierarchyPolicyComponent>(entity);
        const bool cascade = !unpublished && policy && policy->on_parent_destroyed == HierarchyDestroyPolicy::CascadeDestroy;
        if (cascade) {
            // Explicit stack preserves the authored child-list pre-order.
            std::vector<entt::entity> stack{entity};
            while (!stack.empty()) {
                const auto parent = stack.back(); stack.pop_back();
                if (parent != entity) entities.push_back(parent);
                const auto* children = registry.try_get<ChildrenComponent>(parent);
                if (!children) continue;
                for (auto it = children->children.rbegin(); it != children->children.rend(); ++it) {
                    const auto child = resolveEntityRef(registry, *it);
                    if (child != entt::null && registry.valid(child) &&
                        std::find(entities.begin(), entities.end(), child) == entities.end() &&
                        std::find(stack.begin(), stack.end(), child) == stack.end())
                        stack.push_back(child);
                }
            }
        }
        std::vector<LifecycleState::Member> cohort;
        cohort.reserve(entities.size());
        for (const auto candidate : entities) {
            LifecycleState::Member member;
            member.entity = candidate;
            if (component_state) if (const auto* marker = registry.try_get<ScopedEntityMarker>(candidate)) {
                member.handle.m_owner = reinterpret_cast<uintptr_t>(this);
                member.handle.m_world = component_state->token;
                member.handle.m_entity = static_cast<uint32_t>(entt::to_integral(candidate));
                member.handle.m_serial = marker->serial;
            }
            cohort.push_back(member);
        }
        retirement_fence->entities.swap(entities);
        lifecycle_state->cohort.swap(cohort);
        lifecycle_state->current = 0;
        lifecycle_state->completed = 0;
        lifecycle_state->unpublished = unpublished;
        lifecycle_state->constraints_ready = false;
        lifecycle_state->observer_failure = false;
        return processRetirement();
    } catch (const std::bad_alloc&) {
        return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare, current};
    } catch (...) {
        return {LifecycleStatus::Partial, LifecyclePhase::Prepare, current};
    }
}

LifecycleResult world::processRetirement() noexcept
{
    auto& state = *lifecycle_state;
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (state.running) return {LifecycleStatus::Reentrant};
    struct Guard { bool& running; explicit Guard(bool& value) : running(value) { running = true; } ~Guard() { running = false; } } guard(state.running);
    if (!state.constraints_ready) {
        try { physics_system->removeConstraintsTouching(retirement_fence->entities); }
        catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Constraints, state.cohort.front().handle, state.completed}; }
        state.constraints_ready = true;
    }
    while (state.current < state.cohort.size()) {
        auto& member = state.cohort[state.current];
        const auto entity = member.entity;
        if (!registry.valid(entity)) {
            return {LifecycleStatus::Partial, LifecyclePhase::Identity, member.handle, state.completed};
        }
        if (member.phase == LifecyclePhase::Event) {
            if (!state.unpublished && !member.event_attempted) {
                try {
                    EntityDestroyedEvent event;
                    event.ref = entityRefOf(registry, entity);
                    if (const auto* tag = registry.try_get<TagComponent>(entity)) event.tag = tag->name;
                    auto& bus = EventBus::get();
                    (void)bus.sink<EntityDestroyedEvent>();
                    member.event_attempted = true;
                    member.phase = LifecyclePhase::Hierarchy;
                    try { bus.publish(std::move(event)); }
                    catch (...) {
                        state.observer_failure = true;
                        return {LifecycleStatus::ObserverFailure, LifecyclePhase::Event, member.handle, state.completed};
                    }
                } catch (const std::bad_alloc&) {
                    return {LifecycleStatus::AllocationFailure, LifecyclePhase::Event, member.handle, state.completed};
                } catch (...) {
                    return {LifecycleStatus::Partial, LifecyclePhase::Event, member.handle, state.completed};
                }
            } else member.phase = LifecyclePhase::Hierarchy;
        }
        if (member.phase == LifecyclePhase::Hierarchy) {
            try {
                if (!Hierarchy::retireEntity(registry, entity, *retirement_fence))
                    return {LifecycleStatus::Partial, LifecyclePhase::Hierarchy, member.handle, state.completed};
                member.phase = LifecyclePhase::Physics;
            } catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Hierarchy, member.handle, state.completed}; }
        }
        if (member.phase == LifecyclePhase::Physics) {
            try {
                physics_system->removeConstraintsTouching(retirement_fence->entities);
                physics_system->removeBody(entity);
                member.phase = LifecyclePhase::Components;
            } catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Physics, member.handle, state.completed}; }
        }
        if (member.phase == LifecyclePhase::Components) {
            if (const auto* marker = registry.try_get<ScopedEntityMarker>(entity)) {
                const auto removed = marker->store->removeEntity(marker->key);
                if (removed != garden::ecs::ComponentStoreResult::Success && removed != garden::ecs::ComponentStoreResult::NotFound)
                    return {LifecycleStatus::Partial, LifecyclePhase::Components, member.handle, state.completed};
            }
            member.phase = LifecyclePhase::Identity;
        }
        if (member.phase == LifecyclePhase::Identity) {
            try { registry.destroy(entity); }
            catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Identity, member.handle, state.completed}; }
            state.queue.remove_if([&](const auto& request) { return request.handle == member.handle; });
            retirement_fence->pending.erase(entity);
            if (state.unpublished)
                state.rollback.erase(std::remove_if(state.rollback.begin(), state.rollback.end(),
                    [entity](const auto& handle) { return handle.m_entity == static_cast<uint32_t>(entt::to_integral(entity)); }), state.rollback.end());
            ++state.current;
            ++state.completed;
        }
    }
    const auto completed = state.completed;
    const auto status = state.observer_failure ? LifecycleStatus::CompletedWithObserverFailure : LifecycleStatus::Completed;
    state.cohort.clear();
    if (state.unpublished) {
        for (const auto handle : state.rollback)
            if (!registry.valid(static_cast<entt::entity>(handle.m_entity)))
                retirement_fence->pending.erase(static_cast<entt::entity>(handle.m_entity));
        state.rollback.erase(std::remove_if(state.rollback.begin(), state.rollback.end(),
            [&](const auto handle) { return !registry.valid(static_cast<entt::entity>(handle.m_entity)); }), state.rollback.end());
    }
    retirement_fence->entities.clear();
    state.current = state.completed = 0;
    state.observer_failure = false;
    return {status, LifecyclePhase::None, {}, completed};
}

LifecycleResult world::drainRetirements() noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || lifecycle_state->constructing) return {LifecycleStatus::Reentrant};
    size_t completed = 0;
    bool observer_failed = false;
    if (!lifecycle_state->cohort.empty()) {
        auto result = processRetirement();
        if (!result.cleanupComplete()) return result;
        completed += result.completed;
        observer_failed = result.status == LifecycleStatus::CompletedWithObserverFailure;
    }
    while (!lifecycle_state->rollback.empty()) {
        const auto handle = lifecycle_state->rollback.back();
        if (!registry.valid(static_cast<entt::entity>(handle.m_entity))) {
            retirement_fence->pending.erase(static_cast<entt::entity>(handle.m_entity));
            lifecycle_state->rollback.pop_back();
            continue;
        }
        auto result = retireRaw(static_cast<entt::entity>(handle.m_entity), true);
        result.completed += completed;
        if (!result.cleanupComplete()) return result;
        completed = result.completed;
        observer_failed |= result.status == LifecycleStatus::CompletedWithObserverFailure;
    }
    while (!lifecycle_state->queue.empty()) {
        const auto request = lifecycle_state->queue.front();
        const auto handle = request.handle;
        if (validateLifecycleHandle(handle) != LifecycleStatus::Alive) {
            retirement_fence->pending.erase(static_cast<entt::entity>(handle.m_entity));
            lifecycle_state->queue.pop_front();
            continue;
        }
        auto result = retireRaw(static_cast<entt::entity>(handle.m_entity), request.unpublished);
        result.completed += completed;
        if (!result.cleanupComplete()) return result;
        completed = result.completed;
        observer_failed |= result.status == LifecycleStatus::CompletedWithObserverFailure;
    }
    return {observer_failed ? LifecycleStatus::CompletedWithObserverFailure : LifecycleStatus::Completed,
            LifecyclePhase::None, {}, completed};
}

LifecycleResult world::abortUnpublished(ConstructionTransaction& transaction) noexcept
{
    if (transaction.owner != this || transaction.committed)
        return {LifecycleStatus::Invalid};
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || !lifecycle_state->rollback.empty())
        return {LifecycleStatus::Reentrant};
    // Move the transaction's already allocated capability into the world.
    // Retry remains possible even if later cohort preparation cannot allocate.
    lifecycle_state->rollback = std::move(transaction.entities);
    transaction.committed = true;
    for (const auto& handle : lifecycle_state->rollback)
        lifecycle_state->queue.remove_if([&](const auto& queued) { return queued.handle == handle; });
    while (!lifecycle_state->rollback.empty() &&
           !registry.valid(static_cast<entt::entity>(lifecycle_state->rollback.back().m_entity))) {
        retirement_fence->pending.erase(static_cast<entt::entity>(lifecycle_state->rollback.back().m_entity));
        lifecycle_state->rollback.pop_back();
    }
    if (lifecycle_state->rollback.empty()) return {LifecycleStatus::Completed};
    // A construct hook may have left an earlier cohort partially retired. The
    // retained rollback remains fenced while that cohort is resumed by drain.
    if (!lifecycle_state->cohort.empty())
        return {LifecycleStatus::Partial, LifecyclePhase::Prepare};
    return retireRaw(static_cast<entt::entity>(lifecycle_state->rollback.back().m_entity), true);
}
