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
#include <thread>

namespace {
std::atomic<uint64_t> nextComponentWorldToken{1};
uint64_t allocateWorldToken() noexcept {
    auto value = nextComponentWorldToken.load(std::memory_order_relaxed);
    while (value && value != UINT64_MAX) {
        if (nextComponentWorldToken.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0;
}
struct ScopedStoreBinding { garden::ecs::ComponentStore* store = nullptr; };
struct ScopedEntityMarker {
    garden::ecs::ComponentStore* store;
    garden::ecs::ComponentEntityKey key;
    uint64_t serial;
};
bool injectLifecycleFault(const entt::registry& registry, LifecyclePhase phase) noexcept
{
    const auto* hook = registry.ctx().find<garden::ecs::testing::LifecycleFaultHook>();
    return hook && hook->fail && hook->fail(hook->context, phase);
}
bool hasLiveEntities(entt::registry& registry)
{
    const auto entities = registry.storage<entt::entity>().each();
    return entities.begin() != entities.end();
}
void retireScopedEntity(entt::registry& registry, entt::entity entity)
{
    const auto& marker = registry.get<ScopedEntityMarker>(entity);
    const auto* binding = registry.ctx().find<ScopedStoreBinding>();
    if (!binding || !binding->store || marker.store != binding->store ||
        marker.key.slot != static_cast<uint32_t>(entt::to_entity(entity)) ||
        marker.key.generation != static_cast<uint32_t>(entt::to_version(entity)) + 1u) return;
    (void)binding->store->removeEntity(marker.key);
}
}

struct world::ComponentWorldState {
    garden::ecs::ComponentStore store;
    uint64_t token = 0;
    uint64_t nextSerial = 1;
};

struct world::ConstructionState {
    struct Member { EntityRef reference; garden::ecs::EntityHandle handle; std::string tag; bool fresh; };
    std::thread::id owner = std::this_thread::get_id();
    uint64_t next_serial = 1, serial = 0;
    size_t join_depth = 0;
    bool failed = false, transitioning = false, aborted = false;
    std::vector<Member> members;
    std::vector<garden::ecs::EntityHandle> captured;
    std::vector<garden::ecs::EntityHandle> output;
    std::vector<EntityCreatedEvent> events;
    std::vector<garden::ecs::EntityHandle> rollback;
    LifecycleOperationHandle rollback_operation{};
};

struct world::LifecycleState {
    LifecycleOperationHandle operation{};
    LifecycleOperationKind operation_kind = LifecycleOperationKind::None;
    LifecycleResult result{};
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

uint64_t garden::ecs::testing::LifecycleTestAccess::exchangeWorldAllocator(uint64_t value) noexcept
{ return nextComponentWorldToken.exchange(value, std::memory_order_relaxed); }
uint64_t garden::ecs::testing::LifecycleTestAccess::exchangeEntitySerial(world& owner, uint64_t value) noexcept
{
    if (!owner.component_state) return 0;
    return std::exchange(owner.component_state->nextSerial, value);
}

world::world(const PhysicsSystemSettings& physics_settings)
    : simulation_ticks(physics_settings.fixed_delta)
{
    incarnation = allocateWorldToken();
    lifecycle_state = std::make_unique<LifecycleState>();
    construction_state = std::make_unique<ConstructionState>();
    retirement_fence = std::make_unique<RetirementFence>();
    retirement_fence->rollback_context = lifecycle_state.get();
    retirement_fence->rollback_contains = [](const void* context, entt::entity entity) noexcept {
        const auto& rollback = static_cast<const LifecycleState*>(context)->rollback;
        return std::any_of(rollback.begin(), rollback.end(), [entity](const auto& handle) {
            return handle.m_entity == static_cast<uint32_t>(entt::to_integral(entity));
        });
    };
    registry.ctx().emplace<RetirementFenceBinding>(retirement_fence.get());
    registry.ctx().emplace<ScopedStoreBinding>();
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
        std::terminate();
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
    if (std::this_thread::get_id() != construction_state->owner) return {LifecycleStatus::WrongThread};
    if (constructionReentrant()) return {LifecycleStatus::Reentrant};
    if (construction_state->serial) return {LifecycleStatus::Conflict};
    if (lifecycle_state->operation.valid()) return operationConflict();
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || lifecycle_state->constructing) return {LifecycleStatus::Reentrant};
    const uint64_t next_incarnation = allocateWorldToken();
    if (!next_incarnation) return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare};
    if (lifecycle_state->shutdown_complete && !hasLiveEntities(registry)) {
        incarnation = next_incarnation; next_operation_serial = 1;
        return {LifecycleStatus::Completed};
    }
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
        registry.ctx().get<ScopedStoreBinding>().store = nullptr;
        component_state.reset();
        incarnation = next_incarnation;
        next_operation_serial = 1;
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
    if (std::this_thread::get_id() != construction_state->owner) return {LifecycleStatus::WrongThread};
    if (constructionReentrant()) return {LifecycleStatus::Reentrant};
    if (construction_state->serial) return {LifecycleStatus::Conflict};
    if (lifecycle_state->operation.valid()) return operationConflict();
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
        replacement_components->token = incarnation;
        component_state = std::move(replacement_components);
        registry.ctx().get<ScopedStoreBinding>().store = &component_state->store;
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
    const auto raw = resolveEntityRef(registry, entity);
    return isStaged(raw) ? entt::null : raw;
}

bool world::isEntityValid(EntityRef entity) const
{
    return resolveEntity(entity) != entt::null;
}

EntityCreationResult world::createEntity(std::string_view tag) { return createScopedEntity(tag); }

EntityRef world::findEntityByTag(const std::string& name) const
{
    if (name.empty())
        return {};
    entt::entity match = entt::null;
    auto tagged = registry.view<TagComponent>();
    for (const entt::entity entity : tagged) {
        if (isStaged(entity) || tagged.get<TagComponent>(entity).name != name)
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
    if (!registry.valid(entity) || isStaged(entity) || !registry.all_of<ScopedEntityMarker>(entity))
        return ComponentStoreResult::StaleEntity;
    const auto& marker = registry.get<ScopedEntityMarker>(entity);
    if (!handle.m_serial || marker.serial != handle.m_serial || marker.store != &component_state->store ||
        marker.key.slot != static_cast<uint32_t>(entt::to_entity(entity)) ||
        marker.key.generation != static_cast<uint32_t>(entt::to_version(entity)) + 1u) return ComponentStoreResult::StaleEntity;
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
    if (!registry.valid(entity) || isStaged(entity) || !registry.all_of<ScopedEntityMarker>(entity)) return {};
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
    if (active_queries || !incarnation) return {};
    if (reference.isNull()) return {};
    lifecycle_state->shutdown_complete = false;
    entt::entity entity = entt::null;
    for (auto [candidate, identity] : registry.view<IdentityComponent>().each()) {
        if (isStaged(candidate) || identity.guid != reference.guid) continue;
        if (entity != entt::null) return {};
        entity = candidate;
    }
    if (entity == entt::null) return {};
    if (!component_state) { component_state = std::make_unique<ComponentWorldState>(); component_state->token = incarnation; registry.ctx().get<ScopedStoreBinding>().store = &component_state->store; }
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

bool world::isStaged(entt::entity raw) const noexcept {
    if (!construction_state || !construction_state->serial || raw == entt::null) return false;
    for (const auto& h : construction_state->captured)
        if (h.m_entity == static_cast<uint32_t>(entt::to_integral(raw))) return true;
    return false;
}
ConstructionStatus world::validateConstruction(ConstructionHandle h, bool allow_failed) const noexcept {
    const auto& c = *construction_state;
    if (!h.valid()) return ConstructionStatus::InvalidInput;
    if (h.owner_ != this) return ConstructionStatus::WrongWorld;
    if (std::this_thread::get_id() != c.owner) return ConstructionStatus::WrongThread;
    if (h.incarnation_ != incarnation || !c.serial || h.serial_ != c.serial || c.aborted) return ConstructionStatus::StaleTransaction;
    if (active_queries) return ConstructionStatus::QueryActive;
    if (c.transitioning || lifecycle_state->running || lifecycle_state->constructing) return ConstructionStatus::Reentrant;
    if (c.failed && !allow_failed) return ConstructionStatus::TransactionFailed;
    return ConstructionStatus::Completed;
}
ConstructionStatus world::constructionStatus(ConstructionHandle h) const noexcept { return validateConstruction(h); }
world::ConstructionJoinScope::ConstructionJoinScope(world& owner, ConstructionHandle h) noexcept
    : status_(owner.validateConstruction(h)) {
    if (status_ != ConstructionStatus::Completed) return;
    auto& c = *owner.construction_state;
    if (c.join_depth == SIZE_MAX) { status_ = ConstructionStatus::AllocationFailure; return; }
    ++c.join_depth;
    owner_ = &owner;
}
world::ConstructionJoinScope::ConstructionJoinScope(ConstructionJoinScope&& other) noexcept
    : status_(other.status_) {
    if (other.owner_ && std::this_thread::get_id() != other.owner_->construction_state->owner) std::terminate();
    owner_ = std::exchange(other.owner_, nullptr);
    other.status_ = ConstructionStatus::InvalidInput;
}
world::ConstructionJoinScope::~ConstructionJoinScope() noexcept {
    if (!owner_) return;
    auto& c = *owner_->construction_state;
    if (std::this_thread::get_id() != c.owner || !c.join_depth) std::terminate();
    --c.join_depth;
}
world::ConstructionJoinScope world::joinConstruction(ConstructionHandle h) noexcept {
    return ConstructionJoinScope(*this, h);
}
ConstructionBeginResult world::beginConstruction() noexcept {
    auto& c = *construction_state;
    if (std::this_thread::get_id() != c.owner) return {ConstructionStatus::WrongThread};
    if (active_queries) return {ConstructionStatus::QueryActive};
    if (c.transitioning || lifecycle_state->running || lifecycle_state->constructing) return {ConstructionStatus::Reentrant};
    if (c.serial || lifecycle_state->operation.valid() || lifecycle_state->shutting_down || !lifecycle_state->cohort.empty() || !lifecycle_state->queue.empty()) return {ConstructionStatus::Conflict};
    if (!incarnation || !c.next_serial || !next_operation_serial) return {ConstructionStatus::AllocationFailure};
    c.serial = c.next_serial++; c.failed = c.aborted = false; c.members.clear(); c.captured.clear(); c.output.clear(); c.events.clear();
    c.rollback.clear();
    c.rollback_operation.owner_ = this; c.rollback_operation.incarnation_ = incarnation; c.rollback_operation.serial_ = next_operation_serial++;
    ConstructionHandle h; h.owner_ = this; h.incarnation_ = incarnation; h.serial_ = c.serial;
    return {ConstructionStatus::Completed, h};
}
StagedEntityResult world::createStagedEntity(ConstructionHandle h, std::string_view tag) {
    auto status = validateConstruction(h); if (status != ConstructionStatus::Completed) return {status};
    EntityRef ref{};
    try { do { ref.guid = generateEntityGuid(); } while (resolveEntityRef(registry, ref) != entt::null); }
    catch (...) { return {ConstructionStatus::AllocationFailure}; }
    return stageEntity(h, ref, tag, true);
}
StagedEntityResult world::restoreStagedEntity(ConstructionHandle h, EntityRef ref, std::string_view tag) {
    return stageEntity(h, ref, tag, false);
}
StagedEntityResult world::stageEntity(ConstructionHandle h, EntityRef ref, std::string_view tag, bool fresh) {
    auto status = validateConstruction(h); if (status != ConstructionStatus::Completed) return {status};
    if (ref.isNull()) return {ConstructionStatus::InvalidInput};
    for (const auto [raw, identity] : registry.view<IdentityComponent>().each()) { (void)raw; if (identity.guid == ref.guid) return {ConstructionStatus::Conflict}; }
    auto& c = *construction_state;
    const auto before = c.captured.size();
    for (const auto& m : c.members) if (m.reference.guid == ref.guid) return {ConstructionStatus::Conflict};
    try {
        c.members.reserve(c.members.size()+1); c.captured.reserve(c.captured.size()+1); c.rollback.reserve(c.rollback.size()+1); c.output.reserve(c.output.size()+1);
        if (fresh) c.events.reserve(c.events.size()+1);
        lifecycle_state->cohort.reserve(c.captured.size()+1); retirement_fence->entities.reserve(c.captured.size()+1);
        std::string storedTag(tag);
        EntityCreatedEvent event;
        if (fresh) { event.ref = ref; event.tag = storedTag; }
        if (!component_state) { component_state = std::make_unique<ComponentWorldState>(); component_state->token = incarnation; registry.ctx().get<ScopedStoreBinding>().store = &component_state->store; }
        if (!component_state->nextSerial) return {ConstructionStatus::AllocationFailure};
        auto sink = registry.on_destroy<ScopedEntityMarker>(); sink.disconnect<&retireScopedEntity>(); sink.connect<&retireScopedEntity>();
        c.transitioning = true;
        const auto raw = registry.create();
        garden::ecs::EntityHandle issued; issued.m_owner = reinterpret_cast<uintptr_t>(this); issued.m_world = incarnation;
        issued.m_entity = static_cast<uint32_t>(entt::to_integral(raw)); issued.m_serial = component_state->nextSerial++;
        c.captured.push_back(issued); c.rollback.push_back(issued); c.output.push_back(issued);
        c.members.push_back({ref, issued, std::move(storedTag), fresh});
        if (fresh) { event.handle = issued; c.events.push_back(std::move(event)); }
        garden::ecs::ComponentEntityKey key{static_cast<uint32_t>(entt::to_entity(raw)), static_cast<uint32_t>(entt::to_version(raw)) + 1u};
        registry.emplace<ScopedEntityMarker>(raw, &component_state->store, key, issued.m_serial);
        registry.emplace<IdentityComponent>(raw, IdentityComponent{ref.guid});
        if (!tag.empty()) registry.emplace<TagComponent>(raw, TagComponent{std::string(tag)});
        c.transitioning = false; lifecycle_state->shutdown_complete = false;
        StagedEntity staged; staged.transaction_ = h; staged.member_serial_ = c.members.size();
        garden::ecs::ComponentEntityKey verified{};
        if (stagedKey(h, staged, verified) != ConstructionStatus::Completed || !registry.all_of<IdentityComponent>(raw) || registry.get<IdentityComponent>(raw).guid != ref.guid) {
            c.failed = true; return {ConstructionStatus::ComponentFailure, staged, ref, issued};
        }
        size_t count = 0; for (const auto [candidate, identity] : registry.view<IdentityComponent>().each()) { (void)candidate; if (identity.guid == ref.guid) ++count; }
        if (count != 1) { c.failed = true; return {ConstructionStatus::ComponentFailure, staged, ref, issued}; }
        return {ConstructionStatus::Completed, staged, ref, issued};
    } catch (const std::bad_alloc&) {
        const bool accepted=c.transitioning; c.failed |= accepted; c.transitioning=false;
        return {ConstructionStatus::AllocationFailure, {}, accepted && c.captured.size() > before ? ref : EntityRef{}, accepted && c.captured.size() > before ? c.captured.back() : garden::ecs::EntityHandle{}};
    } catch (...) {
        const bool accepted=c.transitioning; c.failed |= accepted; c.transitioning=false;
        return {ConstructionStatus::ComponentFailure, {}, accepted && c.captured.size() > before ? ref : EntityRef{}, accepted && c.captured.size() > before ? c.captured.back() : garden::ecs::EntityHandle{}};
    }
}
ConstructionStatus world::stagedKey(ConstructionHandle h, StagedEntity e, garden::ecs::ComponentEntityKey& key) const noexcept {
    auto status = validateConstruction(h); if (status != ConstructionStatus::Completed) return status;
    if (e.transaction_ != h || !e.member_serial_ || e.member_serial_ > construction_state->members.size()) return ConstructionStatus::Conflict;
    auto raw = static_cast<entt::entity>(construction_state->members[e.member_serial_-1].handle.m_entity);
    if (!registry.valid(raw) || !component_state) return ConstructionStatus::Conflict;
    const auto* marker = registry.try_get<ScopedEntityMarker>(raw);
    if (!marker || marker->serial != construction_state->members[e.member_serial_-1].handle.m_serial || marker->store != &component_state->store || marker->key.slot != static_cast<uint32_t>(entt::to_entity(raw)) || marker->key.generation != static_cast<uint32_t>(entt::to_version(raw)) + 1u) return ConstructionStatus::Conflict;
    key = marker->key; return ConstructionStatus::Completed;
}
bool world::stagedInvariant(ConstructionHandle h, StagedEntity e) const noexcept {
    garden::ecs::ComponentEntityKey key{};
    if (stagedKey(h, e, key) != ConstructionStatus::Completed) return false;
    const auto& member = construction_state->members[e.member_serial_ - 1];
    const auto raw = static_cast<entt::entity>(member.handle.m_entity);
    const auto* identity = registry.try_get<IdentityComponent>(raw);
    if (!identity || identity->guid != member.reference.guid) return false;
    size_t count = 0;
    for (const auto [candidate, current] : registry.view<IdentityComponent>().each()) {
        (void)candidate; if (current.guid == member.reference.guid) ++count;
    }
    return count == 1;
}
entt::entity world::constructionEntity(ConstructionHandle h, StagedEntity e) const noexcept {
    const auto& c = *construction_state;
    if (!h.valid() || h.owner_ != this || h.incarnation_ != incarnation || h.serial_ != c.serial || c.aborted || c.failed ||
        std::this_thread::get_id() != c.owner || active_queries || e.transaction_ != h || !e.member_serial_ || e.member_serial_ > c.members.size()) return entt::null;
    const auto& member = c.members[e.member_serial_-1]; auto raw = static_cast<entt::entity>(member.handle.m_entity);
    if (!registry.valid(raw) || !component_state) return entt::null;
    const auto* marker = registry.try_get<ScopedEntityMarker>(raw);
    if (!marker || marker->serial != member.handle.m_serial || marker->store != &component_state->store ||
        marker->key.slot != static_cast<uint32_t>(entt::to_entity(raw)) || marker->key.generation != static_cast<uint32_t>(entt::to_version(raw)) + 1u) return entt::null;
    return raw;
}
world::ConstructionAuthoringScope::ConstructionAuthoringScope(world& owner, ConstructionHandle h) noexcept : status_(owner.validateConstruction(h)) {
    if (status_ == ConstructionStatus::Completed) { owner_ = &owner; owner.constructionTransition(true); }
}
world::ConstructionAuthoringScope::~ConstructionAuthoringScope() { if (owner_) owner_->constructionTransition(false); }
void world::constructionTransition(bool value) noexcept { construction_state->transitioning = value; }
bool world::constructionReentrant() const noexcept { return construction_state && construction_state->transitioning; }
bool world::constructionActive() const noexcept { return construction_state && construction_state->serial; }
void world::failConstruction(ConstructionHandle h) noexcept { if (validateConstruction(h, true) == ConstructionStatus::Completed) construction_state->failed = true; }
StagedLookupResult world::resolveStagedReference(ConstructionHandle h, EntityRef ref) const noexcept {
    auto status = validateConstruction(h);
    if (status != ConstructionStatus::Completed) {
        switch (status) {
        case ConstructionStatus::InvalidInput: return {StagedLookupStatus::InvalidInput}; case ConstructionStatus::WrongWorld: return {StagedLookupStatus::WrongWorld};
        case ConstructionStatus::WrongThread: return {StagedLookupStatus::WrongThread}; case ConstructionStatus::QueryActive: return {StagedLookupStatus::QueryActive};
        case ConstructionStatus::Reentrant: return {StagedLookupStatus::Reentrant}; case ConstructionStatus::TransactionFailed: return {StagedLookupStatus::TransactionFailed};
        default: return {StagedLookupStatus::StaleTransaction}; }
    }
    if (ref.isNull()) return {StagedLookupStatus::InvalidInput};
    size_t found = 0; for (size_t i=0;i<construction_state->members.size();++i) if (construction_state->members[i].reference.guid == ref.guid) { if (found) return {StagedLookupStatus::Conflict}; found=i+1; }
    if (!found) return {StagedLookupStatus::NotFound};
    StagedEntity e; e.transaction_=h; e.member_serial_=found;
    garden::ecs::ComponentEntityKey key{};
    if (stagedKey(h,e,key) != ConstructionStatus::Completed) return {StagedLookupStatus::Conflict};
    const auto raw = static_cast<entt::entity>(construction_state->members[found-1].handle.m_entity);
    const auto* identity = registry.try_get<IdentityComponent>(raw);
    if (!identity || identity->guid != ref.guid) return {StagedLookupStatus::Conflict};
    size_t count=0; for (const auto [candidate, current] : registry.view<IdentityComponent>().each()) { (void)candidate; if (current.guid == ref.guid) ++count; }
    if (count != 1) return {StagedLookupStatus::Conflict};
    return {StagedLookupStatus::Found,e};
}
ConstructionCommitResult world::commitConstruction(ConstructionHandle h) {
    auto status = validateConstruction(h, true);
    if (status != ConstructionStatus::Completed) return {status};
    if (construction_state->join_depth) return {ConstructionStatus::Conflict};
    status = validateConstruction(h);
    if (status != ConstructionStatus::Completed)
        return status == ConstructionStatus::TransactionFailed
            ? ConstructionCommitResult{status, {}, construction_state->captured}
            : ConstructionCommitResult{status};
    auto& c = *construction_state;
    try {
        for (size_t i=0;i<c.members.size();++i) {
            StagedEntity e; e.transaction_=h; e.member_serial_=i+1; garden::ecs::ComponentEntityKey key{};
            const auto& m=c.members[i]; auto raw=static_cast<entt::entity>(m.handle.m_entity);
            if (stagedKey(h,e,key) != ConstructionStatus::Completed || !registry.all_of<IdentityComponent>(raw) || registry.get<IdentityComponent>(raw).guid != m.reference.guid) { c.failed=true; return {ConstructionStatus::Conflict, {}, c.captured}; }
            size_t count=0; for (const auto [candidate, identity] : registry.view<IdentityComponent>().each()) { (void)candidate; if (identity.guid==m.reference.guid) ++count; }
            if (count!=1) { c.failed=true; return {ConstructionStatus::Conflict, {}, c.captured}; }
        }
        if (!EventBus::get().queueEntityCreatedBatch(std::move(c.events))) return {ConstructionStatus::EventQueueFailure, {}, c.captured};
        c.serial=0; c.members.clear(); c.captured.clear(); c.rollback.clear();
        return {ConstructionStatus::Completed, std::move(c.output), {}};
    } catch (...) { return {ConstructionStatus::EventQueueFailure, {}, c.captured}; }
}
ConstructionAbortResult world::abortConstruction(ConstructionHandle h) noexcept {
    auto status=validateConstruction(h,true); if (status!=ConstructionStatus::Completed) return {status};
    auto& c=*construction_state;
    if (c.join_depth) return {ConstructionStatus::Conflict};
    if (lifecycle_state->operation.valid()) return {ConstructionStatus::Conflict, operationConflict(), c.captured};
    if (c.rollback.empty()) { c.serial=0; c.members.clear(); c.captured.clear(); return {ConstructionStatus::Completed}; }
    auto& state=*lifecycle_state;
    state.rollback = std::move(c.rollback);
    state.operation = c.rollback_operation;
    state.operation_kind = LifecycleOperationKind::UnpublishedRollback;
    operationProgress({LifecycleStatus::Partial, LifecyclePhase::Prepare});
    auto rollback=operationProgress(retireRaw(static_cast<entt::entity>(state.rollback.back().m_entity), true));
    if (!rollback.cleanupComplete() && !rollback.accepted) return {ConstructionStatus::Conflict, rollback, c.captured};
    if (!rollback.operation.valid()) { c.serial=0; c.members.clear(); c.captured.clear(); return {ConstructionStatus::Completed,rollback,{}}; }
    c.aborted=true; return {ConstructionStatus::Completed, rollback, c.captured};
}

namespace {
ScopedRestoreStatus mapConstructionStatus(ConstructionStatus status) noexcept {
    switch (status) {
    case ConstructionStatus::Completed: return ScopedRestoreStatus::Completed;
    case ConstructionStatus::InvalidInput: return ScopedRestoreStatus::InvalidInput;
    case ConstructionStatus::QueryActive: return ScopedRestoreStatus::QueryActive;
    case ConstructionStatus::AllocationFailure: case ConstructionStatus::EventQueueFailure: return ScopedRestoreStatus::AllocationFailure;
    case ConstructionStatus::ComponentFailure: case ConstructionStatus::TransactionFailed: return ScopedRestoreStatus::ConstructionFailure;
    default: return ScopedRestoreStatus::Conflict;
    }
}
}
EntityCreationResult world::createScopedEntity(std::string_view tag) {
    EntityCreationResult result; auto begin=beginConstruction(); result.status=mapConstructionStatus(begin.status);
    if (begin.status!=ConstructionStatus::Completed) return result;
    auto staged=createStagedEntity(begin.transaction,tag); result.status=mapConstructionStatus(staged.status);
    result.reference=staged.reference; result.entity=staged.captured;
    if (staged.status==ConstructionStatus::Completed) {
        auto committed=commitConstruction(begin.transaction); result.status=mapConstructionStatus(committed.status);
        if (committed.status==ConstructionStatus::Completed) { result.entity=committed.entities.front(); return result; }
    }
    auto aborted=abortConstruction(begin.transaction); result.rollback=aborted.rollback;
    if (!aborted.captured.empty()) result.entity=aborted.captured.front();
    return result;
}
ScopedRestoreResult world::restoreScopedEntity(EntityRef reference) { return restoreScopedEntities(std::span<const EntityRef>(&reference,1)); }
ScopedRestoreResult world::restoreScopedEntities(std::span<const EntityRef> references) {
    ScopedRestoreResult result;
    if (active_queries) { result.status = ScopedRestoreStatus::QueryActive; return result; }
    if (construction_state->serial) { result.status = ScopedRestoreStatus::Conflict; return result; }
    const uint64_t nextSerial = component_state ? component_state->nextSerial : 1;
    if (!references.empty() && (!nextSerial || references.size() - 1 > UINT64_MAX - nextSerial)) {
        result.status = ScopedRestoreStatus::AllocationFailure;
        return result;
    }
    try {
        std::unordered_set<EntityGuid, GuidHash> requested; requested.reserve(references.size());
        for (auto ref : references) if (ref.isNull() || !requested.insert(ref.guid).second) { result.status=ScopedRestoreStatus::InvalidInput; return result; }
        for (const auto [raw, identity] : registry.view<IdentityComponent>().each()) { (void)raw; if (requested.count(identity.guid)) { result.status=ScopedRestoreStatus::InvalidInput; return result; } }
    } catch (...) { result.status=ScopedRestoreStatus::AllocationFailure; return result; }
    auto begin=beginConstruction(); result.status=mapConstructionStatus(begin.status);
    if (begin.status!=ConstructionStatus::Completed) return result;
    try { result.entities.reserve(references.size()); }
    catch (...) { result.status=ScopedRestoreStatus::AllocationFailure; (void)abortConstruction(begin.transaction); return result; }
    for (const auto& ref : references) {
        auto staged=restoreStagedEntity(begin.transaction,ref); result.status=mapConstructionStatus(staged.status);
        if (staged.status!=ConstructionStatus::Completed) {
            auto aborted=abortConstruction(begin.transaction); result.rollback=aborted.rollback;
            result.entities.assign(aborted.captured.begin(),aborted.captured.end()); return result;
        }
        result.entities.push_back(staged.captured);
    }
    auto committed=commitConstruction(begin.transaction); result.status=mapConstructionStatus(committed.status);
    if (committed.status==ConstructionStatus::Completed) { result.entities=std::move(committed.entities); return result; }
    auto aborted=abortConstruction(begin.transaction); result.rollback=aborted.rollback;
    result.entities.assign(aborted.captured.begin(),aborted.captured.end()); return result;
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

EntityHandleClassification world::classifyEntityHandle(garden::ecs::EntityHandle h) const noexcept
{
    if (!h.m_owner || !h.m_world || h.m_entity == UINT32_MAX || !h.m_serial) return EntityHandleClassification::Unverifiable;
    if (h.m_owner != reinterpret_cast<uintptr_t>(this)) return EntityHandleClassification::WrongWorld;
    if (h.m_world != incarnation) return EntityHandleClassification::RetiredGone;
    const auto raw = static_cast<entt::entity>(h.m_entity);
    if (!registry.valid(raw)) return EntityHandleClassification::RetiredGone;
    if (isStaged(raw)) return EntityHandleClassification::Unverifiable;
    const auto* marker = registry.try_get<ScopedEntityMarker>(raw);
    if (!marker || !marker->serial || !component_state || marker->store != &component_state->store ||
        marker->key.slot != static_cast<uint32_t>(entt::to_entity(raw)) || marker->key.generation != static_cast<uint32_t>(entt::to_version(raw)) + 1u)
        return EntityHandleClassification::Unverifiable;
    if (marker->serial != h.m_serial) return EntityHandleClassification::RetiredGone;
    if (persistedReference(h).isNull()) return EntityHandleClassification::Unverifiable;
    if (retirement_fence->contains(raw)) return EntityHandleClassification::Retiring;
    if (retirement_fence->isPending(raw)) return EntityHandleClassification::Pending;
    return EntityHandleClassification::Alive;
}

LifecycleResult world::operationConflict() const noexcept
{
    LifecycleResult r{LifecycleStatus::Conflict};
    r.blocked_by = lifecycle_state->operation; r.blocked_by_kind = lifecycle_state->operation_kind;
    return r;
}
LifecycleResult world::operationProgress(LifecycleResult r) noexcept
{
    r.operation = lifecycle_state->operation; r.kind = lifecycle_state->operation_kind; r.accepted = true;
    lifecycle_state->result = r; return r;
}
LifecycleResult world::retirementResult(LifecycleOperationHandle op) const noexcept
{
    if (!op.valid() || op != lifecycle_state->operation || op.owner_ != this || op.incarnation_ != incarnation)
        return {LifecycleStatus::UnknownOperation};
    return lifecycle_state->result;
}
LifecycleResult world::resumeRetirement(LifecycleOperationHandle op) noexcept
{
    auto r = retirementResult(op);
    if (!r.accepted) return r;
    if (std::this_thread::get_id() != construction_state->owner) { r.status = LifecycleStatus::WrongThread; return r; }
    if (constructionReentrant() || lifecycle_state->running || lifecycle_state->constructing) { r.status = LifecycleStatus::Reentrant; return r; }
    if (active_queries) { r.status = LifecycleStatus::QueryActive; return r; }
    if (r.cleanupComplete()) return r;
    if (active_queries) { auto blocked = r; blocked.status = LifecycleStatus::QueryActive; return blocked; }
    if (lifecycle_state->running || lifecycle_state->constructing) { auto blocked = r; blocked.status = LifecycleStatus::Reentrant; return blocked; }
    if (lifecycle_state->cohort.empty() && lifecycle_state->operation_kind == LifecycleOperationKind::UnpublishedRollback && !lifecycle_state->rollback.empty())
        return operationProgress(retireRaw(static_cast<entt::entity>(lifecycle_state->rollback.back().m_entity), true));
    return operationProgress(processRetirement());
}
LifecycleResult world::acknowledgeRetirement(LifecycleOperationHandle op) noexcept
{
    auto r = retirementResult(op);
    if (!r.accepted) return r;
    if (std::this_thread::get_id() != construction_state->owner) { r.status = LifecycleStatus::WrongThread; return r; }
    if (constructionReentrant() || lifecycle_state->running || lifecycle_state->constructing) { r.status = LifecycleStatus::Reentrant; return r; }
    if (active_queries) { r.status = LifecycleStatus::QueryActive; return r; }
    if (!r.cleanupComplete()) { r.status = LifecycleStatus::Conflict; return r; }
    lifecycle_state->operation = {}; lifecycle_state->operation_kind = LifecycleOperationKind::None;
    lifecycle_state->result = {};
    if (construction_state->aborted) { construction_state->serial = 0; construction_state->aborted = false; construction_state->members.clear(); construction_state->captured.clear(); construction_state->output.clear(); construction_state->events.clear(); }
    return r;
}

LifecycleResult world::retireEntities(std::span<const garden::ecs::EntityHandle> handles) noexcept
{ return retireEntitiesImpl(handles, true); }

LifecycleResult world::retireEntitiesImpl(std::span<const garden::ecs::EntityHandle> handles, bool external) noexcept
{
    auto& state = *lifecycle_state;
    if (construction_state->serial) return {LifecycleStatus::Conflict};
    if (state.operation.valid()) return operationConflict();
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (state.running || state.constructing || !state.cohort.empty() || !state.rollback.empty()) return {LifecycleStatus::Reentrant};
    if (handles.empty()) return {};
    if (!incarnation || (external && !next_operation_serial) || injectLifecycleFault(registry, LifecyclePhase::Prepare))
        return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare};
    try {
        std::vector<entt::entity> roots;
        roots.reserve(handles.size());
        for (auto h : handles) {
            const auto cls = classifyEntityHandle(h);
            if (cls != EntityHandleClassification::Alive && cls != EntityHandleClassification::Pending)
                return {cls == EntityHandleClassification::WrongWorld ? LifecycleStatus::WrongWorld : LifecycleStatus::Invalid, LifecyclePhase::Prepare, h};
            const auto raw = static_cast<entt::entity>(h.m_entity);
            if (std::find(roots.begin(), roots.end(), raw) == roots.end()) roots.push_back(raw);
        }
        auto preflight = Hierarchy::preflightRetirementCohort(registry, roots);
        if (preflight.status != Hierarchy::RetirementPreflightStatus::Completed)
            return {preflight.status == Hierarchy::RetirementPreflightStatus::AllocationFailure ? LifecycleStatus::AllocationFailure : LifecycleStatus::Invalid, LifecyclePhase::Prepare};
        std::vector<LifecycleState::Member> cohort;
        cohort.reserve(preflight.cohort.size());
        for (const auto raw : preflight.cohort) {
            const auto* marker = registry.try_get<ScopedEntityMarker>(raw);
            if (!marker) return {LifecycleStatus::Invalid, LifecyclePhase::Prepare};
            garden::ecs::EntityHandle h; h.m_owner = reinterpret_cast<uintptr_t>(this); h.m_world = incarnation;
            h.m_entity = static_cast<uint32_t>(entt::to_integral(raw)); h.m_serial = marker->serial;
            const auto cls = classifyEntityHandle(h);
            if (cls != EntityHandleClassification::Alive && cls != EntityHandleClassification::Pending) return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, h};
            LifecycleState::Member member; member.entity = raw; member.handle = h; cohort.push_back(member);
        }
        std::vector<entt::entity> absorbed;
        for (const auto& request : state.queue) {
            const auto requestClass = classifyEntityHandle(request.handle);
            if (requestClass != EntityHandleClassification::Pending && requestClass != EntityHandleClassification::Alive)
                return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, request.handle};
            const auto raw = static_cast<entt::entity>(request.handle.m_entity);
            auto q = Hierarchy::preflightRetirementCohort(registry, std::span<const entt::entity>(&raw, 1));
            if (q.status != Hierarchy::RetirementPreflightStatus::Completed)
                return {q.status == Hierarchy::RetirementPreflightStatus::AllocationFailure ? LifecycleStatus::AllocationFailure : LifecycleStatus::Invalid, LifecyclePhase::Prepare};
            size_t inside = 0;
            for (auto member : q.cohort) {
                const auto* marker = registry.try_get<ScopedEntityMarker>(member);
                if (!marker) return {LifecycleStatus::Invalid, LifecyclePhase::Prepare};
                garden::ecs::EntityHandle h;
                h.m_owner = reinterpret_cast<uintptr_t>(this); h.m_world = incarnation;
                h.m_entity = static_cast<uint32_t>(entt::to_integral(member)); h.m_serial = marker->serial;
                const auto cls = classifyEntityHandle(h);
                if (cls != EntityHandleClassification::Alive && cls != EntityHandleClassification::Pending)
                    return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, h};
                if (std::find(preflight.cohort.begin(), preflight.cohort.end(), member) != preflight.cohort.end()) ++inside;
            }
            if (inside && inside != q.cohort.size()) return {LifecycleStatus::Conflict, LifecyclePhase::Prepare};
            if (inside) {
                // A queued operation must have the same survivor effects when
                // subsumed by the complete frozen fence.
                for (const auto member : q.cohort) {
                    const auto* children = registry.try_get<ChildrenComponent>(member);
                    if (!children) continue;
                    const auto* policy = registry.try_get<HierarchyPolicyComponent>(member);
                    const auto mode = policy ? policy->on_parent_destroyed : HierarchyDestroyPolicy::OrphanToParent;
                    const auto* parent = registry.try_get<ParentComponent>(member);
                    const auto grandparent = parent && !parent->parent.isNull() ? resolveEntityRef(registry, parent->parent) : entt::null;
                    const bool parentInBatchOnly = grandparent != entt::null &&
                        std::find(preflight.cohort.begin(), preflight.cohort.end(), grandparent) != preflight.cohort.end() &&
                        std::find(q.cohort.begin(), q.cohort.end(), grandparent) == q.cohort.end();
                    for (const auto ref : children->children) {
                        const auto child = resolveEntityRef(registry, ref);
                        if (std::find(q.cohort.begin(), q.cohort.end(), child) != q.cohort.end()) continue;
                        if (std::find(preflight.cohort.begin(), preflight.cohort.end(), child) != preflight.cohort.end() ||
                            (mode == HierarchyDestroyPolicy::OrphanToParent && parentInBatchOnly))
                            return {LifecycleStatus::Conflict, LifecyclePhase::Prepare};
                    }
                }
                absorbed.push_back(raw);
            }
        }
        if (external) {
            state.operation.owner_ = this; state.operation.incarnation_ = incarnation; state.operation.serial_ = next_operation_serial++;
            state.operation_kind = LifecycleOperationKind::PublishedBatch;
        }
        retirement_fence->entities.swap(preflight.cohort); state.cohort.swap(cohort);
        state.current = state.completed = 0; state.unpublished = false; state.constraints_ready = false; state.observer_failure = false;
        for (auto raw : absorbed) { state.queue.remove_if([&](const auto& q) { return q.handle.m_entity == static_cast<uint32_t>(entt::to_integral(raw)); }); retirement_fence->pending.erase(raw); }
        if (!external) return processRetirement();
        operationProgress({LifecycleStatus::Partial, LifecyclePhase::Prepare});
        return operationProgress(processRetirement());
    } catch (const std::bad_alloc&) { return {LifecycleStatus::AllocationFailure, LifecyclePhase::Prepare}; }
      catch (...) { return {LifecycleStatus::InternalFailure, LifecyclePhase::Prepare}; }
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
    if (constructionReentrant()) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    if (construction_state->serial) return {LifecycleStatus::Conflict, LifecyclePhase::None, handle};
    if (lifecycle_state->constructing) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    const auto state = retirementState(handle);
    if (state.status == LifecycleStatus::Pending) return {LifecycleStatus::AlreadyPending, LifecyclePhase::None, handle};
    if (state.status != LifecycleStatus::Alive) return state;
    try {
        const auto raw = static_cast<entt::entity>(handle.m_entity);
        if (!retirement_fence->entities.empty()) {
            auto closure = Hierarchy::preflightRetirementCohort(registry, std::span<const entt::entity>(&raw, 1));
            if (closure.status != Hierarchy::RetirementPreflightStatus::Completed) return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, handle};
            for (auto candidate : closure.cohort) if (retirement_fence->contains(candidate)) return {LifecycleStatus::Conflict, LifecyclePhase::Prepare, handle};
        }
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
    if (constructionReentrant()) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    if (construction_state->serial) return {LifecycleStatus::Conflict, LifecyclePhase::None, handle};
    if (lifecycle_state->operation.valid()) return operationConflict();
    if (active_queries) return {LifecycleStatus::QueryActive, LifecyclePhase::None, handle};
    if (lifecycle_state->running) return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
    const auto status = validateLifecycleHandle(handle);
    if (status != LifecycleStatus::Alive) return {status, LifecyclePhase::None, handle};
    if (!lifecycle_state->cohort.empty()) {
        if (!retirement_fence->contains(static_cast<entt::entity>(handle.m_entity)))
            return {LifecycleStatus::Reentrant, LifecyclePhase::None, handle};
        return processRetirement();
    }
    return retireEntitiesImpl(std::span<const garden::ecs::EntityHandle>(&handle, 1), false);
}

LifecycleResult world::retireRaw(entt::entity entity, bool unpublished) noexcept
{
    if (active_queries) return {LifecycleStatus::QueryActive};
    if (lifecycle_state->running || !lifecycle_state->cohort.empty()) return {LifecycleStatus::Reentrant};
    if (unpublished && !registry.valid(entity)) {
        const auto found = std::find_if(lifecycle_state->rollback.rbegin(), lifecycle_state->rollback.rend(),
            [&](const auto h) { return registry.valid(static_cast<entt::entity>(h.m_entity)); });
        if (found == lifecycle_state->rollback.rend()) {
            const auto completed = lifecycle_state->rollback.size();
            lifecycle_state->rollback.clear();
            return {LifecycleStatus::Completed, LifecyclePhase::None, {}, completed};
        }
        entity = static_cast<entt::entity>(found->m_entity);
    }
    if (entity == entt::null || !registry.valid(entity)) return {LifecycleStatus::Stale};
    garden::ecs::EntityHandle current;
    if (component_state) if (const auto* marker = registry.try_get<ScopedEntityMarker>(entity)) {
        current.m_owner = reinterpret_cast<uintptr_t>(this);
        current.m_world = component_state->token;
        current.m_entity = static_cast<uint32_t>(entt::to_integral(entity));
        current.m_serial = marker->serial;
    }
    if (!unpublished && current.m_serial)
        return retireEntitiesImpl(std::span<const garden::ecs::EntityHandle>(&current, 1), false);
    if (unpublished && !lifecycle_state->rollback.empty()) {
        auto& state = *lifecycle_state;
        retirement_fence->entities.clear(); state.cohort.clear();
        for (auto it = state.rollback.rbegin(); it != state.rollback.rend(); ++it) {
            const auto raw = static_cast<entt::entity>(it->m_entity);
            if (!registry.valid(raw)) continue;
            retirement_fence->entities.push_back(raw);
            LifecycleState::Member member; member.entity = raw; member.handle = *it;
            state.cohort.push_back(member);
        }
        state.current = state.completed = 0; state.unpublished = true;
        state.constraints_ready = false; state.observer_failure = false;
        return processRetirement();
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
        if (!unpublished && current.m_serial) {
            auto preflight = Hierarchy::preflightRetirementCohort(registry, std::span<const entt::entity>(&entity, 1));
            if (preflight.status != Hierarchy::RetirementPreflightStatus::Completed)
                return {preflight.status == Hierarchy::RetirementPreflightStatus::AllocationFailure ? LifecycleStatus::AllocationFailure : LifecycleStatus::Invalid, LifecyclePhase::Prepare, current};
            entities = std::move(preflight.cohort);
            for (const auto raw : entities) {
                const auto* marker = registry.try_get<ScopedEntityMarker>(raw);
                if (!marker) return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, current};
                garden::ecs::EntityHandle h; h.m_owner = reinterpret_cast<uintptr_t>(this); h.m_world = incarnation;
                h.m_entity = static_cast<uint32_t>(entt::to_integral(raw)); h.m_serial = marker->serial;
                const auto cls = classifyEntityHandle(h);
                if (cls != EntityHandleClassification::Alive && cls != EntityHandleClassification::Pending)
                    return {LifecycleStatus::Invalid, LifecyclePhase::Prepare, h};
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
            if (unpublished) {
                for (const auto h : lifecycle_state->rollback) if (h.m_entity == static_cast<uint32_t>(entt::to_integral(candidate))) { member.handle = h; break; }
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
        if (injectLifecycleFault(registry, LifecyclePhase::Constraints))
            return {LifecycleStatus::Partial, LifecyclePhase::Constraints, state.cohort.front().handle, state.completed};
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
            if (injectLifecycleFault(registry, LifecyclePhase::Hierarchy))
                return {LifecycleStatus::Partial, LifecyclePhase::Hierarchy, member.handle, state.completed};
            try {
                if (!Hierarchy::retireEntity(registry, entity, *retirement_fence))
                    return {LifecycleStatus::Partial, LifecyclePhase::Hierarchy, member.handle, state.completed};
                member.phase = LifecyclePhase::Physics;
            } catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Hierarchy, member.handle, state.completed}; }
        }
        if (member.phase == LifecyclePhase::Physics) {
            if (injectLifecycleFault(registry, LifecyclePhase::Physics))
                return {LifecycleStatus::Partial, LifecyclePhase::Physics, member.handle, state.completed};
            try {
                physics_system->removeConstraintsTouching(retirement_fence->entities);
                physics_system->removeBody(entity);
                member.phase = LifecyclePhase::Components;
            } catch (...) { return {LifecycleStatus::Partial, LifecyclePhase::Physics, member.handle, state.completed}; }
        }
        if (member.phase == LifecyclePhase::Components) {
            if (injectLifecycleFault(registry, LifecyclePhase::Components))
                return {LifecycleStatus::Partial, LifecyclePhase::Components, member.handle, state.completed};
            if (component_state && member.handle.m_serial) {
                const garden::ecs::ComponentEntityKey key{static_cast<uint32_t>(entt::to_entity(entity)),
                    static_cast<uint32_t>(entt::to_version(entity)) + 1u};
                const auto removed = component_state->store.removeEntity(key);
                if (removed != garden::ecs::ComponentStoreResult::Success && removed != garden::ecs::ComponentStoreResult::NotFound)
                    return {LifecycleStatus::Partial, LifecyclePhase::Components, member.handle, state.completed};
            }
            member.phase = LifecyclePhase::Identity;
        }
        if (member.phase == LifecyclePhase::Identity) {
            if (injectLifecycleFault(registry, LifecyclePhase::Identity))
                return {LifecycleStatus::Partial, LifecyclePhase::Identity, member.handle, state.completed};
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
    if (lifecycle_state->operation.valid()) return operationConflict();
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
