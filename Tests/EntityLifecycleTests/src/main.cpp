#include "world.hpp"
#include "ECS/ComponentOperations.hpp"
#include "ECS/RetirementFence.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

struct LifecycleValue { int value = 0; };
namespace garden::ecs {
template<> struct ComponentType<LifecycleValue> {
    static constexpr ComponentKey key{"garden.tests.LifecycleValue", 1};
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

bool queueAndQuery()
{
    bool ok = true;
    world w, other;
    const auto a = w.createScopedEntity();
    const auto b = w.createScopedEntity();
    const auto c = w.createScopedEntity();
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
    const auto first = w.createScopedEntity();
    const auto second = w.createScopedEntity();
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
    const auto created = w.createScopedEntity("created tag");
    const auto ref = w.persistedReference(created);
    EventBus::get().flush<EntityCreatedEvent>();
    bool ok = check(created_events.size() == 1 && created_events[0].ref == ref &&
                    created_events[0].tag == "created tag", "normal creation queues tagged event");
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
    const auto* fence = w.registry.ctx().find<RetirementFenceBinding>();
    ok &= check(!w.isEntityValid(result) && construction_side_effects == 1 &&
                w.registry.valid(failed_restore_raw) &&
                w.retirementState(failed_restore_handle).status == LifecycleStatus::Pending &&
                fence && fence->fence->blocks(failed_restore_raw) && events.size() == destroyed_before,
                "failed restore retains fenced unpublished entity after cleanup failure");
    destroy.release();
    ok &= check(w.drainRetirements().cleanupComplete() && !w.registry.valid(failed_restore_raw) &&
                events.size() == destroyed_before, "unpublished rollback retries without destroyed event");
    EventBus::get().flush<EntityCreatedEvent>();
    ok &= check(created_events.empty(), "failed restore publishes no creation event");
    observed_world = nullptr;
    EventBus::get().sink<EntityCreatedEvent>().disconnect<&onCreated>();
    return ok;
}

bool resetAndShutdown()
{
    world w;
    const auto old = w.createScopedEntity();
    auto* address = &w;
    const auto reset = w.resetWorld();
    const auto fresh = w.createScopedEntity();
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
    const bool ok = queueAndQuery() & fifoAndObserverRetry() & resetAndShutdown() & rollbackAndCreationEvent();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyed>();
    EventBus::get().clear();
    return ok ? 0 : 1;
}
