#include "world.hpp"
#include "ECS/ComponentOperations.hpp"
#include "ECS/RetirementFence.hpp"
#include "ECS/Hierarchy.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

struct LifecycleValue { int value = 0; };
struct ProviderValue { int value = 0; };
struct AllocationValue {
    static inline int constructions = 0;
    static inline int destructions = 0;
    explicit AllocationValue(int) { ++constructions; }
    ~AllocationValue() noexcept { ++destructions; }
};
struct EventBusTestAccess {
    static void failNextCreatedBatchAllocation(EventBus& bus) noexcept {
        bus.failNextCreatedBatchAllocation = true;
    }
};
namespace garden::ecs {
namespace testing {
struct ComponentStoreTestAccess {
    static void failNextValueAllocation() noexcept { ComponentStore::failNextValueAllocationForTest(); }
    static void clearValueAllocationFailure() noexcept { ComponentStore::clearValueAllocationFailureForTest(); }
};
}
template<> struct ComponentType<LifecycleValue> {
    static constexpr ComponentKey key{"garden.tests.LifecycleValue", 1};
};
template<> struct ComponentType<ProviderValue> {
    static constexpr ComponentKey key{"garden.tests.ProviderValue", 1};
};
template<> struct ComponentType<AllocationValue> {
    static constexpr ComponentKey key{"garden.tests.AllocationValue", 1};
};
}

namespace {
using garden::ecs::EntityHandle;
world* observed_world = nullptr;
EntityHandle observed_handle{};
std::vector<EntityRef> events;
bool visible = false;
bool throw_once = false;
LifecycleStatus reentrant = LifecycleStatus::Completed;

void onDestroyed(const EntityDestroyedEvent& event)
{
    events.push_back(event.ref);
    if (observed_world && event.ref == observed_world->persistedReference(observed_handle)) {
        LifecycleValue* value = nullptr;
        visible = observed_world->isEntityValid(observed_handle) &&
            observed_world->get<LifecycleValue>(observed_handle, value) == garden::ecs::ComponentStoreResult::Success &&
            value && value->value == 42;
        reentrant = observed_world->drainRetirements().status;
    }
    if (throw_once) { throw_once = false; throw std::runtime_error("fixture observer"); }
}

bool check(bool condition, const char* label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    return condition;
}


bool fixture_ok = true;
bool finishFixtureRollback(world& w, LifecycleResult rollback) {
    if (!rollback.operation.valid()) return true;
    const auto token = rollback.operation;
    for (int i = 0; i < 8 && !rollback.cleanupComplete(); ++i)
        rollback = w.resumeRetirement(token);
    if (!rollback.cleanupComplete()) { fixture_ok = check(false, "fixture rollback completes"); return false; }
    const auto ack = w.acknowledgeRetirement(token);
    if (!ack.cleanupComplete()) { fixture_ok = check(false, "fixture rollback acknowledgement"); return false; }
    return true;
}
struct AllocationFailureGuard {
    ~AllocationFailureGuard() { garden::ecs::testing::ComponentStoreTestAccess::clearValueAllocationFailure(); }
};

struct StagedFixtureGuard {
    world& w;
    ConstructionHandle transaction;
    LifecycleOperationHandle operation{};
    ~StagedFixtureGuard() {
        w.registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
        if (operation.valid()) {
            auto result = w.retirementResult(operation);
            if (result.status != LifecycleStatus::UnknownOperation)
                finishFixtureRollback(w, result);
        }
        if (transaction.valid()) {
            const auto status = w.constructionStatus(transaction);
            if (status != ConstructionStatus::StaleTransaction) {
                const auto aborted = w.abortConstruction(transaction);
                if (aborted.status != ConstructionStatus::Completed) {
                    fixture_ok = check(false, "fixture construction abort");
                } else {
                    finishFixtureRollback(w, aborted.rollback);
                }
            }
        }
    }
};

bool constructionConflictsForFixture(world& w) {
    const auto attempt = w.beginConstruction();
    StagedFixtureGuard guard{w, attempt.transaction};
    return attempt.status == ConstructionStatus::Conflict;
}

EntityCreationResult createForTest(world& w, std::string_view tag = {}) {
    auto result = w.createScopedEntity(tag);
    const bool cleanup = finishFixtureRollback(w, result.rollback);
    if (result.status != ScopedRestoreStatus::Completed || !cleanup || result.rollback.operation.valid()) {
        fixture_ok = check(false, "fixture entity creation succeeds");
        return {};
    }
    return result;
}
EntityCreationResult createRefForTest(world& w) {
    auto result = w.createEntity();
    const bool cleanup = finishFixtureRollback(w, result.rollback);
    if (result.status != ScopedRestoreStatus::Completed || !cleanup || result.rollback.operation.valid()) {
        fixture_ok = check(false, "fixture reference creation succeeds");
        return {};
    }
    return result;
}

bool queueAndQuery()
{
    bool ok = true;
    world w, other;
    const auto a = createForTest(w).entity;
    const auto b = createForTest(w).entity;
    const auto c = createForTest(w).entity;
    for (auto h : {a, b, c})
        ok &= w.add<LifecycleValue>(h, LifecycleValue{42}) == garden::ecs::ComponentStoreResult::Success;
    int callbacks = 0;
    bool guarded = false;
    auto query = w.query<LifecycleValue>([&](EntityHandle current, LifecycleValue&) {
        ++callbacks;
        for (auto h : {a, b, c}) if (h != current)
            ok &= w.requestRetirement(h).status == LifecycleStatus::Queued;
        guarded = w.drainRetirements().status == LifecycleStatus::QueryActive &&
                  w.retireEntity(current).status == LifecycleStatus::QueryActive &&
                  w.shutdown().status == LifecycleStatus::QueryActive &&
                  w.resetWorld().status == LifecycleStatus::QueryActive;
    });
    ok &= check(query == garden::ecs::ComponentStoreResult::Success && callbacks == 1 && guarded,
                "query-time requests filter later yields and guard structural retirement");
    const auto pending = w.retirementState(a).status == LifecycleStatus::Pending ? a : b;
    ok &= check(w.requestRetirement(pending).status == LifecycleStatus::AlreadyPending &&
                other.retirementState(pending).status == LifecycleStatus::WrongWorld &&
                w.retirementState(EntityHandle{}).status == LifecycleStatus::Invalid,
                "requests are idempotent and reject wrong-world and invalid handles");
    ok &= check(w.drainRetirements().cleanupComplete(), "query requests drain successfully");
    return ok;
}

bool fifoAndObserverRetry()
{
    bool ok = true;
    world w;
    const auto first = createForTest(w).entity;
    const auto second = createForTest(w).entity;
    const auto first_ref = w.persistedReference(first);
    const auto second_ref = w.persistedReference(second);
    ok &= w.add<LifecycleValue>(first, LifecycleValue{42}) == garden::ecs::ComponentStoreResult::Success;
    observed_world = &w; observed_handle = first; visible = false; events.clear(); throw_once = true;
    w.requestRetirement(first);
    w.requestRetirement(second);
    auto failed = w.drainRetirements();
    ok &= check(failed.status == LifecycleStatus::ObserverFailure && failed.phase == LifecyclePhase::Event &&
                failed.entity == first && failed.completed == 0 && w.isEntityValid(first) && visible &&
                reentrant == LifecycleStatus::Reentrant,
                "observer failure reports progress with entity readable and rejects reentrant drain");
    auto retry = w.drainRetirements();
    ok &= check(retry.status == LifecycleStatus::CompletedWithObserverFailure && retry.completed == 2 &&
                !w.isEntityValid(first) && !w.isEntityValid(second) && events.size() == 2 &&
                events[0] == first_ref && events[1] == second_ref,
                "retry preserves FIFO and does not repeat the attempted event");
    observed_world = nullptr;
    return ok;
}

EntityHandle failed_restore_handle{};
entt::entity failed_restore_raw = entt::null;
bool fail_destroy_once = false;
int construction_side_effects = 0;
std::vector<EntityCreatedEvent> created_events;
void onCreated(const EntityCreatedEvent& event) { created_events.push_back(event); }
std::vector<std::string> created_attempts;
bool created_throw = false;
bool created_reenter = false;
bool created_reentry_rejected = false;
bool created_enqueue = false;
bool created_clear = false;
struct LegacyFifoEvent {};
void onLegacyFifo(const LegacyFifoEvent&) { created_attempts.push_back("legacy"); }
void onCreatedFifo(const EntityCreatedEvent& event)
{
    created_attempts.push_back(event.tag);
    if (created_reenter) {
        created_reenter = false;
        try { EventBus::get().flush<EntityCreatedEvent>(); }
        catch (const std::logic_error&) { created_reentry_rejected = true; }
    }
    if (created_enqueue) {
        created_enqueue = false;
        EventBus::get().emplace<EntityCreatedEvent>(EntityRef{}, "new", EntityHandle{});
    }
    if (created_clear) {
        created_clear = false;
        EventBus::get().clear();
        EventBus::get().queue(EntityCreatedEvent{EntityRef{}, "after clear", {}});
    }
    if (created_throw) { created_throw = false; throw std::runtime_error("created observer"); }
}

bool createdFifoContract()
{
    auto& bus = EventBus::get();
    bus.clear();
    created_attempts.clear();
    bus.sink<EntityCreatedEvent>().connect<&onCreatedFifo>();
    bus.sink<LegacyFifoEvent>().connect<&onLegacyFifo>();
    std::vector<EntityCreatedEvent> batch{{EntityRef{}, "one", {}}, {EntityRef{}, "two", {}}};
    bus.queue(EntityCreatedEvent{EntityRef{}, "pending", {}});
    EventBusTestAccess::failNextCreatedBatchAllocation(bus);
    bool ok = !bus.queueEntityCreatedBatch(std::move(batch)) && batch.size() == 2 &&
        batch[0].tag == "one" && batch[1].tag == "two";
    bus.flush<EntityCreatedEvent>();
    ok &= created_attempts == std::vector<std::string>{"pending"};
    created_attempts.clear();
    ok &= bus.queueEntityCreatedBatch(std::move(batch)) && batch.empty();
    bus.queue(EntityCreatedEvent{EntityRef{}, "three", {}});
    bus.queue(LegacyFifoEvent{});
    created_throw = true;
    try { bus.flush<EntityCreatedEvent>(); ok = false; }
    catch (const std::runtime_error&) {}
    ok &= created_attempts == std::vector<std::string>{"one"};
    created_reenter = true;
    created_reentry_rejected = false;
    created_enqueue = true;
    bus.flush();
    ok &= created_reentry_rejected && created_attempts == std::vector<std::string>{"one", "two", "three", "legacy"};
    bus.flush<EntityCreatedEvent>();
    ok &= created_attempts == std::vector<std::string>{"one", "two", "three", "legacy", "new"};
    bus.queue(EntityCreatedEvent{EntityRef{}, "clear", {}});
    bus.queue(EntityCreatedEvent{EntityRef{}, "cancelled", {}});
    created_clear = true;
    bus.flush<EntityCreatedEvent>();
    ok &= created_attempts == std::vector<std::string>{"one", "two", "three", "legacy", "new", "clear"};
    bus.flush<EntityCreatedEvent>();
    ok &= created_attempts == std::vector<std::string>{"one", "two", "three", "legacy", "new", "clear", "after clear"};
    bus.sink<EntityCreatedEvent>().disconnect<&onCreatedFifo>();
    bus.sink<LegacyFifoEvent>().disconnect<&onLegacyFifo>();
    bus.clear();
    return check(ok, "creation batch allocation failure, FIFO, observer failure, snapshot, clear and reentry");
}

bool typedFlushPreservesOtherQueues()
{
    auto& bus = EventBus::get();
    bus.clear();
    created_attempts.clear();
    bus.sink<EntityCreatedEvent>().connect<&onCreatedFifo>();
    bus.sink<LegacyFifoEvent>().connect<&onLegacyFifo>();

    EntityCreatedEvent copied{EntityRef{}, "copied", {}};
    bus.queue(copied);
    bus.emplace<EntityCreatedEvent>(EntityRef{}, "emplaced", EntityHandle{});
    bus.queue(LegacyFifoEvent{});
    bus.flush<LegacyFifoEvent>();
    bool ok = created_attempts == std::vector<std::string>{"legacy"};

    bus.queue(LegacyFifoEvent{});
    bus.flush<EntityCreatedEvent>();
    ok &= created_attempts == std::vector<std::string>{"legacy", "copied", "emplaced"};
    bus.flush<LegacyFifoEvent>();
    ok &= created_attempts == std::vector<std::string>{"legacy", "copied", "emplaced", "legacy"};

    bus.sink<EntityCreatedEvent>().disconnect<&onCreatedFifo>();
    bus.sink<LegacyFifoEvent>().disconnect<&onLegacyFifo>();
    bus.clear();
    return check(ok, "typed flush preserves other event queues");
}
void onRestoreConstruct(entt::registry& registry, entt::entity entity)
{
    failed_restore_raw = entity;
    failed_restore_handle = observed_world->scopedEntity(entityRefOf(registry, entity));
    ++construction_side_effects;
    throw std::runtime_error("construction failure after side effect");
}
void onRestoreDestroy(entt::registry&, entt::entity)
{
    if (fail_destroy_once) { fail_destroy_once = false; throw std::runtime_error("retry retirement"); }
}

bool rollbackAndCreationEvent()
{
    EventBus::get().clear();
    created_events.clear();
    EventBus::get().sink<EntityCreatedEvent>().connect<&onCreated>();
    world w;
    const auto created = createForTest(w, "created tag").entity;
    const auto ref = w.persistedReference(created);
    EventBus::get().flush<EntityCreatedEvent>();
    bool ok = check(created_events.size() == 1 && created_events[0].ref == ref &&
                    created_events[0].tag == "created tag" && created_events[0].handle == created,
                    "normal creation queues tagged event with world-scoped handle");
    created_events.clear();
    observed_world = &w;
    construction_side_effects = 0;
    fail_destroy_once = true;
    auto construct = w.registry.on_construct<IdentityComponent>().connect<&onRestoreConstruct>();
    auto destroy = w.registry.on_destroy<IdentityComponent>().connect<&onRestoreDestroy>();
    const auto reference = EntityRef{generateEntityGuid()};
    const size_t destroyed_before = events.size();
    const auto result = w.restoreScopedEntity(reference);
    construct.release();
    const auto captured = result.entities.empty() ? EntityHandle{} : result.entities.front();
    const auto* fence = w.registry.ctx().find<RetirementFenceBinding>();
    ok &= check(result.status == ScopedRestoreStatus::ConstructionFailure && result.rollback.operation.valid() && construction_side_effects == 1 &&
                w.registry.valid(failed_restore_raw) &&
                failed_restore_handle == EntityHandle{} && w.retirementState(captured).status == LifecycleStatus::Pending &&
                fence && fence->fence->blocks(failed_restore_raw) && events.size() == destroyed_before,
                "failed restore retains fenced unpublished entity after cleanup failure");
    destroy.release();
    ok &= check(w.resumeRetirement(result.rollback.operation).cleanupComplete() &&
                w.acknowledgeRetirement(result.rollback.operation).cleanupComplete() && !w.registry.valid(failed_restore_raw) &&
                events.size() == destroyed_before, "unpublished rollback retries without destroyed event");
    EventBus::get().flush<EntityCreatedEvent>();
    ok &= check(created_events.empty(), "failed restore publishes no creation event");
    observed_world = nullptr;
    EventBus::get().sink<EntityCreatedEvent>().disconnect<&onCreated>();
    return ok;
}

world* batch_world = nullptr;
EntityHandle batch_a{}, batch_b{};
bool first_event_fenced = false;
int batch_events = 0;
void onBatchDestroyed(const EntityDestroyedEvent&) {
    if (++batch_events == 1) first_event_fenced =
        batch_world->classifyEntityHandle(batch_a) == EntityHandleClassification::Retiring &&
        batch_world->classifyEntityHandle(batch_b) == EntityHandleClassification::Retiring &&
        batch_world->requestRetirement(batch_b).status == LifecycleStatus::AlreadyPending;
}

bool batchFenceAndPartialQueue()
{
    world w;
    const auto parent = createForTest(w);
    const auto child = createForTest(w);
    const auto unrelated = createForTest(w);
    const auto parentRaw = resolveEntityRef(w.registry, parent.reference);
    const auto childRaw = resolveEntityRef(w.registry, child.reference);
    w.registry.emplace<HierarchyPolicyComponent>(parentRaw).on_parent_destroyed = HierarchyDestroyPolicy::CascadeDestroy;
    bool ok = Hierarchy::attach(w.registry, childRaw, parentRaw);
    ok &= w.requestRetirement(parent.entity).status == LifecycleStatus::Queued;
    std::vector<EntityHandle> subset{child.entity};
    auto rejected = w.retireEntities(subset);
    ok &= check(rejected.status == LifecycleStatus::Conflict && !rejected.accepted &&
        w.registry.valid(parentRaw) && w.registry.valid(childRaw) &&
        w.retirementState(parent.entity).status == LifecycleStatus::Pending,
        "partial queued cascade intersection rejects batch without changing queue");
    batch_world = &w; batch_a = child.entity; batch_b = unrelated.entity; batch_events = 0; first_event_fenced = false;
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onBatchDestroyed>();
    std::vector<EntityHandle> full{parent.entity, unrelated.entity};
    auto accepted = w.retireEntities(full);
    ok &= check(accepted.cleanupComplete() && accepted.completed == 3 && first_event_fenced,
        "first event observes every frozen member fenced");
    if (accepted.operation.valid()) w.acknowledgeRetirement(accepted.operation);
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onBatchDestroyed>();
    batch_world = nullptr;
    return ok;
}

void removeMarkerDuringConstruction(entt::registry& registry, entt::entity entity)
{
    auto* identity = &registry.storage<IdentityComponent>();
    for (auto&& [id, storage] : registry.storage()) {
        (void)id;
        if (&storage != identity && storage.contains(entity)) { storage.erase(entity); break; }
    }
}
bool markerlessRollbackToken()
{
    world w;
    auto connection = w.registry.on_construct<IdentityComponent>().connect<&removeMarkerDuringConstruction>();
    const auto restored = w.restoreScopedEntity(EntityRef{generateEntityGuid()});
    connection.release();
    bool ok = restored.status == ScopedRestoreStatus::ConstructionFailure && restored.entities.size() == 1 &&
        restored.rollback.accepted && restored.rollback.kind == LifecycleOperationKind::UnpublishedRollback &&
        restored.rollback.operation.valid();
    if (restored.rollback.operation.valid()) ok &= finishFixtureRollback(w, restored.rollback);
    return check(ok && w.classifyEntityHandle(restored.entities.empty() ? EntityHandle{} : restored.entities.front()) ==
        EntityHandleClassification::RetiredGone, "markerless failed restore returns durable rollback token");
}

struct FaultFixture { LifecyclePhase phase; bool fired = false; };
bool injectOnce(void* p, LifecyclePhase phase) noexcept {
    auto& f = *static_cast<FaultFixture*>(p);
    if (!f.fired && f.phase == phase) { f.fired = true; return true; }
    return false;
}
int phaseEvents = 0;
void onPhaseDestroyed(const EntityDestroyedEvent&) { ++phaseEvents; }
bool phaseRetryCoverage()
{
    bool ok = true;
    for (const auto phase : {LifecyclePhase::Prepare, LifecyclePhase::Constraints, LifecyclePhase::Hierarchy,
                             LifecyclePhase::Physics, LifecyclePhase::Components, LifecyclePhase::Identity}) {
        world w;
        const auto a = createForTest(w).entity;
        const auto b = createForTest(w).entity;
        FaultFixture fault{phase};
        w.registry.ctx().emplace<garden::ecs::testing::LifecycleFaultHook>(
            garden::ecs::testing::LifecycleFaultHook{&fault, &injectOnce});
        phaseEvents = 0;
        EventBus::get().sink<EntityDestroyedEvent>().connect<&onPhaseDestroyed>();
        std::vector<EntityHandle> submitted{a, b};
        auto result = w.retireEntities(submitted);
        if (phase == LifecyclePhase::Prepare) {
            ok &= result.status == LifecycleStatus::AllocationFailure && !result.accepted &&
                w.isEntityValid(a) && w.isEntityValid(b);
            result = w.retireEntities(submitted);
        } else {
            ok &= result.accepted && result.status == LifecycleStatus::Partial && result.phase == phase &&
                w.classifyEntityHandle(a) == EntityHandleClassification::Retiring &&
                w.classifyEntityHandle(b) == EntityHandleClassification::Retiring;
            result = w.resumeRetirement(result.operation);
        }
        ok &= result.cleanupComplete() && result.completed == 2 && phaseEvents == 2 &&
            w.acknowledgeRetirement(result.operation).cleanupComplete();
        w.registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
        EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onPhaseDestroyed>();
    }
    return check(ok, "injected lifecycle phase failures preserve fence and resume without repeated events");
}

bool singleRootAndHierarchyEffects()
{
    bool ok = true;
    {
        world w;
        const auto parent = createForTest(w), child = createForTest(w);
        auto pr = resolveEntityRef(w.registry, parent.reference);
        auto cr = resolveEntityRef(w.registry, child.reference);
        w.registry.emplace<HierarchyPolicyComponent>(pr).on_parent_destroyed = HierarchyDestroyPolicy::CascadeDestroy;
        ok &= Hierarchy::attach(w.registry, cr, pr);
        ok &= w.requestRetirement(parent.entity).status == LifecycleStatus::Queued;
        auto blocked = w.retireEntity(child.entity);
        ok &= blocked.status == LifecycleStatus::Conflict && w.isEntityValid(child.entity) &&
            w.retirementState(parent.entity).status == LifecycleStatus::Pending;
        ok &= w.drainRetirements().cleanupComplete();
    }
    {
        world w;
        const auto grand = createForTest(w), parent = createForTest(w), child = createForTest(w);
        auto gr = resolveEntityRef(w.registry, grand.reference);
        auto pr = resolveEntityRef(w.registry, parent.reference);
        auto cr = resolveEntityRef(w.registry, child.reference);
        ok &= Hierarchy::attach(w.registry, pr, gr) && Hierarchy::attach(w.registry, cr, pr);
        ok &= w.requestRetirement(parent.entity).status == LifecycleStatus::Queued;
        std::vector<EntityHandle> roots{grand.entity, parent.entity};
        auto blocked = w.retireEntities(roots);
        ok &= blocked.status == LifecycleStatus::Conflict && !blocked.accepted &&
            w.isEntityValid(grand.entity) && w.isEntityValid(parent.entity) && w.isEntityValid(child.entity);
        w.registry.emplace<HierarchyPolicyComponent>(pr).on_parent_destroyed = HierarchyDestroyPolicy::CascadeDestroy;
        auto compatible = w.retireEntities(roots);
        ok &= compatible.cleanupComplete() && compatible.completed == 3 &&
            w.acknowledgeRetirement(compatible.operation).cleanupComplete();
    }
    return check(ok, "single-root overlap and queued hierarchy effects require compatible cohorts");
}

bool queuedSurvivorIsReparented()
{
    world w;
    const auto grand = createForTest(w), parent = createForTest(w), child = createForTest(w);
    const auto grandRaw = resolveEntityRef(w.registry, grand.reference);
    const auto parentRaw = resolveEntityRef(w.registry, parent.reference);
    const auto childRaw = resolveEntityRef(w.registry, child.reference);
    bool ok = Hierarchy::attach(w.registry, parentRaw, grandRaw) &&
        Hierarchy::attach(w.registry, childRaw, parentRaw) &&
        w.requestRetirement(child.entity).status == LifecycleStatus::Queued;
    const auto retired = w.retireEntity(parent.entity);
    const auto* link = w.registry.try_get<ParentComponent>(childRaw);
    const auto* children = w.registry.try_get<ChildrenComponent>(grandRaw);
    ok &= retired.status == LifecycleStatus::Completed && !w.registry.valid(parentRaw) &&
        w.registry.valid(childRaw) && link && link->parent == grand.reference && children &&
        std::find(children->children.begin(), children->children.end(), child.reference) != children->children.end() &&
        w.retirementState(child.entity).status == LifecycleStatus::Pending;
    const auto drained = w.drainRetirements();
    ok &= drained.status == LifecycleStatus::Completed && !w.registry.valid(childRaw) && w.registry.valid(grandRaw);
    return check(ok, "queued survivor is reparented before its FIFO retirement");
}

bool providerPinnedDuringRetry()
{
    using namespace garden::ecs;
    std::array<uint8_t, 32> digest{}; digest[0] = 0xad;
    const auto owner = ComponentOwnerId::module(digest, 87);
    garden::ecs::ComponentDescriptor descriptor{ComponentType<ProviderValue>::key, owner, sizeof(ProviderValue), alignof(ProviderValue),
        componentCapabilities<ProviderValue>(), makeComponentOperations<ProviderValue>()};
    ComponentId id{};
    auto& catalog = ComponentCatalog::instance();
    if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success)
        return check(false, "register retry fixture provider");
    bool ok = true;
    {
        world w;
        const auto entity = createForTest(w).entity;
        ok &= w.add<ProviderValue>(entity, ProviderValue{42}) == ComponentStoreResult::Success;
        FaultFixture fault{LifecyclePhase::Components};
        w.registry.ctx().emplace<testing::LifecycleFaultHook>(testing::LifecycleFaultHook{&fault, &injectOnce});
        const std::vector<EntityHandle> roots{entity};
        auto result = w.retireEntities(roots);
        ok &= result.status == LifecycleStatus::Partial && result.phase == LifecyclePhase::Components &&
            catalog.unregisterOwner(owner) == ComponentCatalogResult::InUse;
        result = w.resumeRetirement(result.operation);
        ok &= result.cleanupComplete() && w.acknowledgeRetirement(result.operation).cleanupComplete();
        w.registry.ctx().erase<testing::LifecycleFaultHook>();
    }
    ok &= catalog.unregisterOwner(owner) == ComponentCatalogResult::Success;
    return check(ok, "provider stays pinned across incomplete component retirement");
}

bool stagedAllocationFailureRetry()
{
    using namespace garden::ecs;
    std::array<uint8_t, 32> digest{}; digest[0] = 0xae;
    const auto owner = ComponentOwnerId::module(digest, 88);
    garden::ecs::ComponentDescriptor descriptor{ComponentType<AllocationValue>::key, owner, sizeof(AllocationValue), alignof(AllocationValue),
        componentCapabilities<AllocationValue>(), makeComponentOperations<AllocationValue>()};
    auto& catalog = ComponentCatalog::instance();
    ComponentId id{};
    if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success)
        return check(false, "register allocation failure fixture provider");
    AllocationValue::constructions = AllocationValue::destructions = 0;
    bool ok = true;
    {
        world w;
        auto begin = w.beginConstruction();
        StagedFixtureGuard stagedGuard{w, begin.transaction};
        AllocationFailureGuard allocationGuard;
        auto first = w.createStagedEntity(begin.transaction);
        auto second = w.createStagedEntity(begin.transaction);
        auto added = w.add<AllocationValue>(begin.transaction, first.entity, 1);
        const bool ready = begin.status == ConstructionStatus::Completed && first.status == ConstructionStatus::Completed &&
            second.status == ConstructionStatus::Completed && added.component_status == ComponentStoreResult::Success;
        ok &= ready;
        if (ready) {
            testing::ComponentStoreTestAccess::failNextValueAllocation();
            auto failed = w.add<AllocationValue>(begin.transaction, second.entity, 2);
            ok &= failed.component_status == ComponentStoreResult::ResourceExhausted &&
                failed.transaction_status == ConstructionStatus::TransactionFailed &&
                w.constructionStatus(begin.transaction) == ConstructionStatus::TransactionFailed &&
                w.commitConstruction(begin.transaction).status == ConstructionStatus::TransactionFailed &&
                AllocationValue::constructions == 1 && AllocationValue::destructions == 0;
        }
        FaultFixture fault{LifecyclePhase::Hierarchy};
        w.registry.ctx().emplace<testing::LifecycleFaultHook>(testing::LifecycleFaultHook{&fault, &injectOnce});
        auto aborted = w.abortConstruction(begin.transaction);
        const auto token = aborted.rollback.operation;
        stagedGuard.operation = token;
        ok &= aborted.status == ConstructionStatus::Completed && aborted.rollback.accepted && token.valid() &&
            aborted.rollback.status == LifecycleStatus::Partial && aborted.rollback.phase == LifecyclePhase::Hierarchy &&
            fault.fired && catalog.unregisterOwner(owner) == ComponentCatalogResult::InUse &&
            constructionConflictsForFixture(w) &&
            AllocationValue::constructions == 1 && AllocationValue::destructions == 0;
        if (token.valid()) {
            auto resumed = w.resumeRetirement(token);
            ok &= resumed.cleanupComplete() && resumed.completed == 2 && AllocationValue::destructions == 1;
            ok &= constructionConflictsForFixture(w);
            ok &= w.acknowledgeRetirement(token).cleanupComplete() &&
                w.retirementResult(token).status == LifecycleStatus::UnknownOperation &&
                w.acknowledgeRetirement(token).status == LifecycleStatus::UnknownOperation;
        }
        w.registry.ctx().erase<testing::LifecycleFaultHook>();
        auto reuse = w.beginConstruction();
        StagedFixtureGuard reuseGuard{w, reuse.transaction};
        auto restored = w.restoreStagedEntity(reuse.transaction, second.reference);
        ok &= reuse.status == ConstructionStatus::Completed && restored.status == ConstructionStatus::Completed;
        auto cleanup = w.abortConstruction(reuse.transaction);
        reuseGuard.operation = cleanup.rollback.operation;
        ok &= finishFixtureRollback(w, cleanup.rollback);
    }
    ok &= AllocationValue::constructions == 1 && AllocationValue::destructions == 1;
    const auto unregistered = catalog.unregisterOwner(owner);
    const auto unregisteredAgain = catalog.unregisterOwner(owner);
    ok &= unregistered == ComponentCatalogResult::Success && unregisteredAgain == ComponentCatalogResult::NotFound;
    return check(ok, "staged value allocation failure poisons construction and preserves retry ownership");
}

bool exhaustionBeforeCreation()
{
    using garden::ecs::testing::LifecycleTestAccess;
    world w;
    const auto sentinel = createForTest(w);
    const auto before = w.registry.storage<entt::entity>().free_list();
    const auto serial = LifecycleTestAccess::exchangeEntitySerial(w, UINT64_MAX);
    const EntityRef refs[] = {{generateEntityGuid()}, {generateEntityGuid()}};
    const auto rejected = w.restoreScopedEntities(refs);
    LifecycleTestAccess::exchangeEntitySerial(w, serial);
    const bool unexpectedRollbackCleaned = finishFixtureRollback(w, rejected.rollback);
    bool ok = unexpectedRollbackCleaned && rejected.status == ScopedRestoreStatus::AllocationFailure && rejected.entities.empty() &&
        !rejected.rollback.operation.valid() && w.registry.storage<entt::entity>().free_list() == before &&
        w.isEntityValid(sentinel.entity);
    const auto allocator = LifecycleTestAccess::exchangeWorldAllocator(UINT64_MAX);
    {
        world exhausted;
        LifecycleTestAccess::exchangeWorldAllocator(allocator);
        auto result = exhausted.createScopedEntity();
        ok &= result.status == ScopedRestoreStatus::AllocationFailure && !result.rollback.operation.valid() &&
            exhausted.registry.storage<entt::entity>().free_list() == 0;
    }
    return check(ok, "serial and world incarnation exhaustion reject before entity creation");
}

bool staleQueuedRootRejectsBatch()
{
    world w;
    const auto queued = createForTest(w);
    const auto target = createForTest(w);
    const auto raw = resolveEntityRef(w.registry, queued.reference);
    const bool queuedOk = w.requestRetirement(queued.entity).status == LifecycleStatus::Queued;
    w.registry.destroy(raw); // raw bypass deliberately corrupts the queued ownership record
    const std::vector<EntityHandle> submitted{target.entity};
    const auto result = w.retireEntities(submitted);
    return check(queuedOk && result.status == LifecycleStatus::Invalid && !result.accepted &&
        w.isEntityValid(target.entity) && !result.operation.valid(),
        "stale disjoint queued root rejects batch before publication");
}

bool batchOwnership()
{
    world w, other;
    auto a = createForTest(w).entity;
    auto b = createForTest(w).entity;
    auto queued = createForTest(w).entity;
    w.requestRetirement(a); w.requestRetirement(b); w.requestRetirement(queued);
    std::vector<EntityHandle> inputs{a, b, a};
    auto batch = w.retireEntities(inputs);
    bool ok = check(batch.accepted && batch.operation.valid() && batch.kind == LifecycleOperationKind::PublishedBatch &&
        batch.cleanupComplete() && batch.completed == 2 && w.retirementState(queued).status == LifecycleStatus::Pending,
        "batch absorbs independent queued roots and retains disjoint FIFO request");
    auto conflict = w.retireEntities({});
    ok &= check(conflict.status == LifecycleStatus::Conflict && !conflict.accepted &&
        conflict.blocked_by == batch.operation && w.drainRetirements().status == LifecycleStatus::Conflict &&
        w.resetWorld().status == LifecycleStatus::Conflict && w.shutdown().status == LifecycleStatus::Conflict,
        "terminal batch blocks new operations before side effects");
    ok &= check(other.retirementResult(batch.operation).status == LifecycleStatus::UnknownOperation &&
        w.resumeRetirement(batch.operation).completed == 2 && w.acknowledgeRetirement(batch.operation).cleanupComplete() &&
        w.retirementResult(batch.operation).status == LifecycleStatus::UnknownOperation && w.drainRetirements().cleanupComplete(),
        "batch token is world bound and requires explicit terminal acknowledgement");
    return ok;
}

bool resetAndShutdown()
{
    world w;
    const auto old = createForTest(w).entity;
    auto* address = &w;
    const auto reset = w.resetWorld();
    const auto fresh = createForTest(w).entity;
    bool ok = check(reset.cleanupComplete() && address == &w &&
                    w.retirementState(old).status == LifecycleStatus::Stale && w.isEntityValid(fresh),
                    "reset preserves world address and expires old handles");
    const auto shut = w.shutdown();
    ok &= check(shut.cleanupComplete() && w.shutdown().cleanupComplete() && !w.isEntityValid(fresh),
                "shutdown retires entities and is idempotent");
    return ok;
}
}

int main()
{
    using namespace garden::ecs;
    auto& catalog = ComponentCatalog::instance();
    const auto owner = ComponentOwnerId::engineCore();
    garden::ecs::ComponentDescriptor descriptor{ComponentType<LifecycleValue>::key, owner, sizeof(LifecycleValue), alignof(LifecycleValue),
                                   componentCapabilities<LifecycleValue>(), makeComponentOperations<LifecycleValue>()};
    ComponentId id{};
    if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success) return 1;
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyed>();
    const bool ok = queueAndQuery() & fifoAndObserverRetry() & resetAndShutdown() & rollbackAndCreationEvent() & createdFifoContract() & typedFlushPreservesOtherQueues() & batchOwnership() & batchFenceAndPartialQueue() & staleQueuedRootRejectsBatch() & singleRootAndHierarchyEffects() & phaseRetryCoverage() & queuedSurvivorIsReparented() & markerlessRollbackToken() & exhaustionBeforeCreation() & providerPinnedDuringRetry() & stagedAllocationFailureRetry();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyed>();
    EventBus::get().clear();
    return ok && fixture_ok ? 0 : 1;
}
