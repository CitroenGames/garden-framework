#include "Components/IdentityComponent.hpp"
#include "Components/Components.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
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

int g_created_count = 0;
int g_destroyed_count = 0;
EntityGuid g_last_destroyed_guid{};
bool g_destroyed_seen_valid = false;
entt::registry* g_observe_registry = nullptr;
EntityLifecycle* g_observe_lifecycle = nullptr;
EntityLifecycle* g_clear_on_destroy = nullptr;
entt::registry* g_destroy_in_handler = nullptr;
entt::entity g_handler_entity = entt::null;
entt::entity g_handler_replacement = entt::null;
EntityLifecycle* g_reentrant_lifecycle = nullptr;
entt::entity g_reentrant_entity = entt::null;
bool g_reentrant_clear = false;
EntityLifecycle* g_throw_lifecycle = nullptr;
entt::entity g_throw_enqueue = entt::null;
std::vector<EntityGuid> g_throw_events;
EntityLifecycle* g_drain_peer_on_destroy = nullptr;

void onCreated(const EntityCreatedEvent&) { ++g_created_count; }
void onDestroyed(const EntityDestroyedEvent& e)
{
    ++g_destroyed_count;
    g_last_destroyed_guid = e.ref.guid;
    if (g_observe_lifecycle)
    {
        entt::entity resolved = g_observe_lifecycle->resolve(e.ref);
        g_destroyed_seen_valid = resolved != entt::null && g_observe_registry && g_observe_registry->valid(resolved);
    }
}

void onDestroyedAndClear(const EntityDestroyedEvent& e)
{
    onDestroyed(e);
    if (g_clear_on_destroy)
        g_clear_on_destroy->clear();
}

void onDestroyedAndReplace(const EntityDestroyedEvent& e)
{
    onDestroyed(e);
    if (g_destroy_in_handler && g_destroy_in_handler->valid(g_handler_entity))
    {
        g_destroy_in_handler->destroy(g_handler_entity);
        g_handler_replacement = g_destroy_in_handler->create();
        g_destroy_in_handler->emplace<IdentityComponent>(
            g_handler_replacement, EntityGuid{0x6101, 0x6102});
    }
}

void onDestroyedAndReenter(const EntityDestroyedEvent& e)
{
    onDestroyed(e);
    // Limit recursion in a broken implementation so the test reports failure.
    if (g_destroyed_count == 1 && g_reentrant_lifecycle)
    {
        if (g_reentrant_clear)
            g_reentrant_lifecycle->clear();
        g_reentrant_lifecycle->destroyImmediate(g_reentrant_entity);
    }
}

void onDestroyedAndThrowOnce(const EntityDestroyedEvent& e)
{
    g_throw_events.push_back(e.ref.guid);
    if (g_throw_lifecycle)
    {
        g_throw_lifecycle->destroy(g_throw_enqueue);
        g_throw_lifecycle = nullptr;
        throw std::runtime_error("observer failure");
    }
}

void onDestroyedClearAndThrow(const EntityDestroyedEvent& e)
{
    g_throw_events.push_back(e.ref.guid);
    if (g_clear_on_destroy)
    {
        g_clear_on_destroy->clear();
        g_clear_on_destroy = nullptr;
        throw std::runtime_error("observer cleared and failed");
    }
}

void onDestroyedDrainPeerAndThrow(const EntityDestroyedEvent& e)
{
    g_throw_events.push_back(e.ref.guid);
    if (g_drain_peer_on_destroy)
    {
        auto* peer = g_drain_peer_on_destroy;
        g_drain_peer_on_destroy = nullptr;
        peer->drainDestroyQueue();
        throw std::runtime_error("observer drained peer and failed");
    }
}

void resetBus()
{
    EventBus::get().sink<EntityCreatedEvent>().disconnect<&onCreated>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyed>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedAndClear>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedAndReplace>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedAndReenter>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedAndThrowOnce>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedClearAndThrow>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyedDrainPeerAndThrow>();
    EventBus::get().clear();
    g_created_count = 0;
    g_destroyed_count = 0;
    g_destroyed_seen_valid = false;
    g_last_destroyed_guid = EntityGuid{};
    g_observe_lifecycle = nullptr;
    g_observe_registry = nullptr;
    g_clear_on_destroy = nullptr;
    g_destroy_in_handler = nullptr;
    g_handler_entity = entt::null;
    g_handler_replacement = entt::null;
    g_reentrant_lifecycle = nullptr;
    g_reentrant_entity = entt::null;
    g_reentrant_clear = false;
    g_throw_lifecycle = nullptr;
    g_throw_enqueue = entt::null;
    g_throw_events.clear();
    g_drain_peer_on_destroy = nullptr;
}

// L1: create -> resolve(refOf(e))==e; destroy -> drain -> resolve==null, valid==false
// Also deterministic-seed check.
bool testL1()
{
    const std::string name = "L1 create/resolve/destroy/drain";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 42);
    entt::entity e = life.create("hero");
    if (!reg.valid(e)) return fail(name, "created entity invalid");
    EntityRef ref = life.refOf(e);
    if (ref.isNull()) return fail(name, "refOf returned null ref");
    if (life.resolve(ref) != e) return fail(name, "resolve(refOf(e)) != e");
    if (life.index().resolve(ref) != e) return fail(name, "index resolve mismatch");
    // guid string round-trip
    EntityGuid parsed{};
    if (!guidFromString(guidToString(ref.guid), parsed) || parsed != ref.guid)
        return fail(name, "guid string round-trip failed");
    // deterministic seed: a fresh registry with same seed yields same first guid
    {
        entt::registry reg2;
        EntityLifecycle life2(reg2, 42);
        entt::entity e2 = life2.create();
        if (life2.refOf(e2).guid != ref.guid)
            return fail(name, "deterministic seed 42 did not reproduce first guid");
    }
    life.destroy(e);
    if (!life.isPendingDestroy(e)) return fail(name, "not pending after destroy");
    if (!reg.valid(e)) return fail(name, "entity invalid before drain (must stay valid)");
    if (life.pendingCount() != 1) return fail(name, "pendingCount != 1");
    life.drainDestroyQueue();
    if (reg.valid(e)) return fail(name, "entity still valid after drain");
    if (life.resolve(ref) != entt::null) return fail(name, "resolve != null after drain");
    return pass(name);
}

// L2: destroy during view iteration over 100 entities
bool testL2()
{
    const std::string name = "L2 destroy during view iteration (100)";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 7);
    std::vector<entt::entity> entities;
    for (int i = 0; i < 100; ++i)
        entities.push_back(life.create());
    int visited = 0;
    auto view = life.aliveView<IdentityComponent>();
    for (entt::entity e : view)
    {
        life.destroy(e); // deferred; must not invalidate iteration
        ++visited;
    }
    if (visited != 100) return fail(name, "visited != 100, got " + std::to_string(visited));
    // aliveView must now exclude all pending
    int alive = 0;
    for (entt::entity e : life.aliveView<IdentityComponent>()) { (void)e; ++alive; }
    if (alive != 0) return fail(name, "aliveView did not exclude PendingDestroy");
    life.drainDestroyQueue();
    for (entt::entity e : entities)
        if (reg.valid(e)) return fail(name, "entity still valid after drain");
    return pass(name);
}

// L3 (adapted to ref-resolution level; serializer migration of reflected
// entt::entity fields to EntityRef is out of scope for this slice):
// A holds EntityRef to B; destroy B; create C (recycle attempt);
// A's ref must resolve to null, never to C.
bool testL3()
{
    const std::string name = "L3 stale EntityRef never resolves to recycled entity";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 99);
    entt::entity a = life.create("A");
    entt::entity b = life.create("B");
    EntityRef a_ref_to_b = life.refOf(b); // what a reflected EntityRef field would store
    (void)a;
    life.destroy(b);
    life.drainDestroyQueue();
    if (reg.valid(b)) return fail(name, "B still valid after drain");
    std::vector<entt::entity> created;
    for (int i = 0; i < 50; ++i)
        created.push_back(life.create("C"));
    if (life.resolve(a_ref_to_b) != entt::null)
        return fail(name, "stale ref resolved to a recycled entity");
    for (entt::entity c : created)
    {
        if (life.refOf(c).guid == a_ref_to_b.guid)
            return fail(name, "new entity reused B guid");
        if (life.resolve(life.refOf(c)) != c)
            return fail(name, "new entity does not resolve via its own ref");
    }
    return pass(name);
}

// L4: events (create deferred, destroyed published before destroy)
bool testL4()
{
    const std::string name = "L4 lifecycle events with GUIDs";
    resetBus();
    EventBus::get().sink<EntityCreatedEvent>().connect<&onCreated>();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyed>();
    entt::registry reg;
    EntityLifecycle life(reg, 5);
    g_observe_registry = &reg;
    g_observe_lifecycle = &life;
    std::vector<entt::entity> es;
    for (int i = 0; i < 3; ++i) es.push_back(life.create("e"));
    if (g_created_count != 0) return fail(name, "created events not deferred before flush");
    EventBus::get().flush();
    if (g_created_count != 3) return fail(name, "expected 3 created after flush");
    EntityGuid b_guid = life.refOf(es[1]).guid;
    life.destroy(es[1]);
    life.drainDestroyQueue();
    if (g_destroyed_count != 1) return fail(name, "expected 1 destroyed event");
    if (g_last_destroyed_guid != b_guid) return fail(name, "destroyed event guid mismatch");
    if (!g_destroyed_seen_valid) return fail(name, "destroyed handler did not see entity valid (must fire before destroy)");
    if (reg.valid(es[1])) return fail(name, "entity valid after drain");
    resetBus();
    return pass(name);
}

// L5: legacy bare registry.create + manual IdentityComponent emplace (hook index)
bool testL5()
{
    const std::string name = "L5 legacy manual IdentityComponent indexed via hook";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 3);
    entt::entity e = reg.create();
    EntityGuid g{0x1234, 0x5678};
    reg.emplace<IdentityComponent>(e, g);
    if (life.resolve(EntityRef{g}) != e) return fail(name, "hook did not index legacy entity");
    if (life.refOf(e).guid != g) return fail(name, "refOf mismatch for legacy entity");
    reg.destroy(e);
    if (life.resolve(EntityRef{g}) != entt::null) return fail(name, "index not erased on destroy");
    return pass(name);
}

// L6: double destroy + destroy(null)
bool testL6()
{
    const std::string name = "L6 double destroy and destroy(null) are no-ops";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 11);
    entt::entity e = life.create();
    life.destroy(e);
    life.destroy(e);
    life.destroy(entt::null);
    life.destroyImmediate(entt::null);
    if (life.pendingCount() != 1) return fail(name, "pendingCount != 1 after double destroy");
    life.drainDestroyQueue();
    life.drainDestroyQueue();
    if (reg.valid(e)) return fail(name, "entity valid after drain");
    return pass(name);
}

// L7: a service constructed after an Editor/PIE registry clone backfills IDs.
bool testL7()
{
    const std::string name = "L7 constructor backfills cloned registry identities";
    resetBus();
    entt::registry reg;
    const entt::entity first = reg.create();
    const entt::entity second = reg.create();
    const EntityGuid first_guid{0x101, 0x201};
    const EntityGuid second_guid{0x102, 0x202};
    reg.emplace<IdentityComponent>(first, first_guid);
    reg.emplace<IdentityComponent>(second, second_guid);

    EntityLifecycle life(reg, 12);
    if (life.resolve(EntityRef{first_guid}) != first)
        return fail(name, "first pre-existing identity was not indexed");
    if (life.resolve(EntityRef{second_guid}) != second)
        return fail(name, "second pre-existing identity was not indexed");
    return pass(name);
}

// L8: backfill must reserve an ambiguous GUID, including for fresh generation.
bool testL8()
{
    const std::string name = "L8 duplicate backfill stays ambiguous and reserves GUID";
    resetBus();
    constexpr uint64_t seed = 731;
    uint64_t state = seed;
    EntityGuid duplicate_guid{splitmix64Next(state), splitmix64Next(state)};
    if (duplicate_guid.isNull()) duplicate_guid.lo = 1;

    entt::registry reg;
    const entt::entity first = reg.create();
    const entt::entity second = reg.create();
    const entt::entity third = reg.create();
    reg.emplace<IdentityComponent>(first, duplicate_guid);
    reg.emplace<IdentityComponent>(second, duplicate_guid);
    reg.emplace<IdentityComponent>(third, duplicate_guid);

    EntityLifecycle life(reg, seed);
    const EntityRef duplicate_ref{duplicate_guid};
    if (life.resolve(duplicate_ref) != entt::null || life.index().resolve(duplicate_ref) != entt::null)
        return fail(name, "duplicate backfill resolved to an arbitrary entity");
    const entt::entity fresh = life.create();
    if (life.refOf(fresh).isNull() || life.refOf(fresh).guid == duplicate_guid ||
        life.resolve(life.refOf(fresh)) != fresh)
        return fail(name, "fresh generation reused an ambiguous GUID");

    reg.remove<IdentityComponent>(second);
    if (life.resolve(duplicate_ref) != entt::null)
        return fail(name, "two remaining owners were no longer ambiguous");
    reg.destroy(third);
    if (life.resolve(duplicate_ref) != first || life.index().resolve(duplicate_ref) != first)
        return fail(name, "sole remaining owner was not restored after destroy");
    reg.destroy(first);
    if (life.resolve(duplicate_ref) != entt::null || life.index().by_guid.count(duplicate_guid) != 0)
        return fail(name, "last removed owner left an index entry");
    return pass(name);
}

// L9: duplicates arriving after construction must not replace the first owner.
bool testL9()
{
    const std::string name = "L9 duplicate construct and removal maintain index";
    resetBus();
    entt::registry reg;
    EntityLifecycle life(reg, 13);
    const EntityGuid guid{0x2201, 0x2202};
    const EntityRef ref{guid};
    const entt::entity first = reg.create();
    reg.emplace<IdentityComponent>(first, guid);
    if (life.resolve(ref) != first)
        return fail(name, "first owner was not indexed");

    const entt::entity second = reg.create();
    const entt::entity third = reg.create();
    reg.emplace<IdentityComponent>(second, guid);
    reg.emplace<IdentityComponent>(third, guid);
    if (life.resolve(ref) != entt::null || life.index().resolve(ref) != entt::null)
        return fail(name, "duplicate construct resolved to an arbitrary entity");
    reg.destroy(second);
    if (life.resolve(ref) != entt::null)
        return fail(name, "two remaining owners were no longer ambiguous");
    reg.remove<IdentityComponent>(third);
    if (life.resolve(ref) != first || life.index().resolve(ref) != first)
        return fail(name, "sole owner was not restored after component removal");
    reg.remove<IdentityComponent>(first);
    if (life.resolve(ref) != entt::null || life.index().by_guid.count(guid) != 0)
        return fail(name, "last component removal left an index entry");

    // EnTT can publish every on_destroy signal before erasing any component
    // during a bulk clear. The index must retire each owner exactly once.
    const entt::entity fourth = reg.create();
    const entt::entity fifth = reg.create();
    reg.emplace<IdentityComponent>(fourth, guid);
    reg.emplace<IdentityComponent>(fifth, guid);
    reg.clear();
    if (life.resolve(ref) != entt::null || life.index().by_guid.count(guid) != 0)
        return fail(name, "bulk clear retained an ambiguous GUID");
    const entt::entity after_clear = reg.create();
    reg.emplace<IdentityComponent>(after_clear, guid);
    if (life.resolve(ref) != after_clear)
        return fail(name, "identity could not be indexed after bulk clear");
    return pass(name);
}

// L10: an index inconsistency activates the same fail-closed path used after
// an indexing allocation failure, without adding an allocator test API.
bool testL10()
{
    const std::string name = "L10 degraded identity index scans safely";
    resetBus();
    constexpr uint64_t seed = 821;
    uint64_t state = seed;
    EntityGuid guid{splitmix64Next(state), splitmix64Next(state)};
    if (guid.isNull()) guid.lo = 1;
    const EntityRef ref{guid};
    entt::registry reg;
    EntityLifecycle life(reg, seed);
    const entt::entity first = reg.create();
    reg.emplace<IdentityComponent>(first, guid);

    // Simulate an ambiguous entry with missing owner tracking. The next
    // construct detects this inconsistency and degrades the index.
    life.index().by_guid.at(guid) = entt::null;
    const entt::entity second = reg.create();
    reg.emplace<IdentityComponent>(second, guid);
    if (life.index().resolve(ref) != entt::null || life.resolve(ref) != entt::null)
        return fail(name, "degraded lookup accepted an ambiguous identity");
    const entt::entity fresh = life.create();
    if (life.refOf(fresh).isNull() || life.refOf(fresh).guid == guid ||
        life.resolve(life.refOf(fresh)) != fresh)
        return fail(name, "degraded generation reused a live GUID");
    reg.destroy(second);
    if (life.index().resolve(ref) != entt::null || life.resolve(ref) != first)
        return fail(name, "degraded lookup did not recover the unique owner");
    life.clear();
    if (life.index().resolve(ref) != first || life.resolve(ref) != first)
        return fail(name, "clear did not rebuild a healthy index from live identities");
    reg.clear();
    if (life.resolve(ref) != entt::null || life.index().resolve(ref) != entt::null)
        return fail(name, "degraded lookup retained a cleared owner");
    return pass(name);
}

// L11: multiple services sharing one index receive the same destroy signals.
bool testL11()
{
    const std::string name = "L11 shared lifecycle destroy callbacks are idempotent";
    resetBus();
    entt::registry reg;
    EntityLifecycle first_life(reg, 31);
    EntityLifecycle second_life(reg, 32);
    const EntityGuid unique_guid{0x3101, 0x3102};
    const EntityRef unique_ref{unique_guid};
    const entt::entity unique = reg.create();
    reg.emplace<IdentityComponent>(unique, unique_guid);
    if (first_life.index().resolve(unique_ref) != unique || second_life.resolve(unique_ref) != unique)
        return fail(name, "unique identity was not indexed by shared services");
    reg.destroy(unique);
    if (first_life.index().by_guid.count(unique_guid) != 0)
        return fail(name, "unique destroy left an index entry");

    const EntityGuid guid{0x3201, 0x3202};
    const EntityRef ref{guid};
    const entt::entity first = reg.create();
    const entt::entity second = reg.create();
    const entt::entity third = reg.create();
    reg.emplace<IdentityComponent>(first, guid);
    if (first_life.index().resolve(ref) != first)
        return fail(name, "unique destroy degraded the shared index");
    reg.emplace<IdentityComponent>(second, guid);
    reg.emplace<IdentityComponent>(third, guid);
    reg.remove<IdentityComponent>(second);
    if (first_life.index().resolve(ref) != entt::null || second_life.resolve(ref) != entt::null)
        return fail(name, "two remaining owners were not ambiguous");
    reg.destroy(third);
    if (first_life.index().resolve(ref) != first || second_life.resolve(ref) != first)
        return fail(name, "shared destroy callbacks did not restore the sole owner");
    reg.remove<IdentityComponent>(first);
    if (first_life.index().by_guid.count(guid) != 0)
        return fail(name, "shared component-removal callbacks left an index entry");

    const entt::entity next = reg.create();
    reg.emplace<IdentityComponent>(next, unique_guid);
    if (first_life.index().resolve(unique_ref) != next || second_life.resolve(unique_ref) != next)
        return fail(name, "duplicate removal degraded the shared index");
    const entt::entity bulk_duplicate = reg.create();
    reg.emplace<IdentityComponent>(bulk_duplicate, unique_guid);
    reg.clear();
    if (first_life.index().by_guid.count(unique_guid) != 0)
        return fail(name, "shared bulk-clear callbacks retained an index entry");
    const entt::entity after_clear = reg.create();
    reg.emplace<IdentityComponent>(after_clear, unique_guid);
    if (first_life.index().resolve(unique_ref) != after_clear || second_life.resolve(unique_ref) != after_clear)
        return fail(name, "shared bulk clear degraded the index");
    return pass(name);
}

// L12: clear must retain the identity contract while the registry is live.
bool testL12()
{
    const std::string name = "L12 clear rebuilds live and duplicate identities";
    resetBus();
    constexpr uint64_t seed = 931;
    uint64_t state = seed;
    EntityGuid guid{splitmix64Next(state), splitmix64Next(state)};
    if (guid.isNull()) guid.lo = 1;
    const EntityRef ref{guid};
    entt::registry reg;
    EntityLifecycle life(reg, seed);
    const entt::entity first = reg.create();
    reg.emplace<IdentityComponent>(first, guid);
    life.clear();
    if (life.index().resolve(ref) != first || life.resolve(ref) != first)
        return fail(name, "clear lost a live unique identity");
    const entt::entity second = reg.create();
    reg.emplace<IdentityComponent>(second, guid);
    life.clear();
    if (life.index().resolve(ref) != entt::null || life.resolve(ref) != entt::null)
        return fail(name, "clear or duplicate construct resolved an ambiguous identity");
    const entt::entity fresh = life.create();
    if (life.refOf(fresh).isNull() || life.refOf(fresh).guid == guid || life.resolve(life.refOf(fresh)) != fresh)
        return fail(name, "clear allowed generation to reuse a live ambiguous GUID");
    reg.remove<IdentityComponent>(second);
    if (life.index().resolve(ref) != first || life.resolve(ref) != first)
        return fail(name, "unique lookup was not restored after duplicate removal");
    return pass(name);
}

// L13: clear cancels pending destruction and permits a later request.
bool testL13()
{
    const std::string name = "L13 clear cancels pending destruction";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyed>();
    entt::registry reg;
    EntityLifecycle life(reg, 41);
    const entt::entity first = life.create("first");
    const entt::entity second = life.create("second");
    const entt::entity unqueued = life.create("unqueued");
    const EntityRef first_ref = life.refOf(first);
    const EntityRef second_ref = life.refOf(second);
    life.destroy(first);
    life.destroy(second);
    reg.emplace<PendingDestroy>(unqueued);
    if (life.pendingCount() != 2 || !life.isPendingDestroy(first) || !life.isPendingDestroy(second))
        return fail(name, "pending setup failed");

    life.clear();
    if (life.pendingCount() != 0 || life.isPendingDestroy(first) || life.isPendingDestroy(second) ||
        life.isPendingDestroy(unqueued) || g_destroyed_count != 0 ||
        life.resolve(first_ref) != first || life.resolve(second_ref) != second)
        return fail(name, "clear did not cancel pending markers and preserve live identities");
    unsigned alive = 0;
    for (entt::entity entity : life.aliveView<IdentityComponent>())
    {
        if (entity == first || entity == second || entity == unqueued)
            ++alive;
    }
    if (alive != 3)
        return fail(name, "cancelled entities were not returned to the alive view");

    life.destroy(first);
    if (life.pendingCount() != 1 || !life.isPendingDestroy(first))
        return fail(name, "cancelled entity could not be queued again");
    life.drainDestroyQueue();
    if (reg.valid(first) || !reg.valid(second) || !reg.valid(unqueued) ||
        life.resolve(first_ref) != entt::null || life.resolve(second_ref) != second ||
        g_destroyed_count != 1 || g_last_destroyed_guid != first_ref.guid)
        return fail(name, "redestroy did not retire only the requeued entity");
    resetBus();
    return pass(name);
}

// L14: a clear cancels queues owned by every lifecycle service for a registry.
bool testL14()
{
    const std::string name = "L14 clear invalidates peer lifecycle queues";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyed>();
    entt::registry reg;
    EntityLifecycle first_life(reg, 51);
    EntityLifecycle second_life(reg, 52);
    const entt::entity first = first_life.create("first");
    const entt::entity second = first_life.create("second");
    const entt::entity third = first_life.create("third");
    first_life.destroy(first);
    first_life.destroy(second);
    second_life.clear();
    if (first_life.pendingCount() != 0 || first_life.isPendingDestroy(first) ||
        first_life.isPendingDestroy(second))
        return fail(name, "peer clear did not cancel the stale queue and markers");
    first_life.drainDestroyQueue();
    if (!reg.valid(first) || !reg.valid(second) || g_destroyed_count != 0)
        return fail(name, "draining a stale queue destroyed a cancelled entity");

    second_life.destroy(first);
    first_life.drainDestroyQueue();
    if (!reg.valid(first) || second_life.pendingCount() != 1 || g_destroyed_count != 0)
        return fail(name, "a stale peer drained a fresh request");
    second_life.drainDestroyQueue();
    if (reg.valid(first) || !reg.valid(second) || g_destroyed_count != 1)
        return fail(name, "fresh peer destruction failed");

    first_life.destroy(second);
    first_life.drainDestroyQueue();
    if (reg.valid(second) || g_destroyed_count != 2)
        return fail(name, "fresh destruction after peer clear failed");
    first_life.destroy(third);
    second_life.clear();
    first_life.destroy(third); // discard the stale queue before requeueing
    if (first_life.pendingCount() != 1)
        return fail(name, "requeue retained stale work or lost the new request");
    first_life.drainDestroyQueue();
    if (reg.valid(third) || g_destroyed_count != 3)
        return fail(name, "requeued destruction did not run exactly once");
    resetBus();
    return pass(name);
}

// L15: a destruction event commits its entity even if a handler clears later work.
bool testL15()
{
    const std::string name = "L15 reentrant clear preserves published destruction";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndClear>();
    entt::registry reg;
    EntityLifecycle life(reg, 61);
    g_observe_registry = &reg;
    g_observe_lifecycle = &life;
    g_clear_on_destroy = &life;
    const entt::entity first = life.create("first");
    const entt::entity second = life.create("second");
    reg.emplace<TransformComponent>(first);
    reg.emplace<TransformComponent>(second);
    if (!Hierarchy::attach(reg, second, first))
        return fail(name, "could not attach the surviving child");
    const EntityRef first_ref = life.refOf(first);
    const EntityRef second_ref = life.refOf(second);
    life.destroy(first);
    life.destroy(second);
    life.drainDestroyQueue();
    if (g_destroyed_count != 1 || g_last_destroyed_guid != first_ref.guid ||
        !g_destroyed_seen_valid || reg.valid(first) || life.resolve(first_ref) != entt::null)
        return fail(name, "published entity was not destroyed and removed from the index");
    if (!reg.valid(second) || life.isPendingDestroy(second) || life.pendingCount() != 0 ||
        life.resolve(second_ref) != second || reg.all_of<ParentComponent>(second))
        return fail(name, "later entity did not survive clear and hierarchy cleanup");
    unsigned alive = 0;
    for (entt::entity entity : life.aliveView<IdentityComponent>())
        if (entity == second) ++alive;
    if (alive != 1)
        return fail(name, "surviving entity was not in the alive view");
    resetBus();
    return pass(name);
}

// L16: a handler may already retire the published entity and recycle its slot.
bool testL16()
{
    const std::string name = "L16 handler retirement does not destroy replacement";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndReplace>();
    entt::registry reg;
    EntityLifecycle life(reg, 62);
    const entt::entity original = life.create("original");
    const EntityRef original_ref = life.refOf(original);
    g_destroy_in_handler = &reg;
    g_handler_entity = original;
    life.destroy(original);
    life.drainDestroyQueue();
    const EntityRef replacement_ref{EntityGuid{0x6101, 0x6102}};
    if (g_destroyed_count != 1 || reg.valid(original) || life.resolve(original_ref) != entt::null ||
        g_handler_replacement == entt::null || !reg.valid(g_handler_replacement) ||
        life.resolve(replacement_ref) != g_handler_replacement)
        return fail(name, "handler-retired entity or its replacement has incorrect lifetime");
    resetBus();
    return pass(name);
}

// L17: immediate destruction must tolerate retirement by the event handler.
bool testL17()
{
    const std::string name = "L17 immediate handler retirement preserves replacement";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndReplace>();
    entt::registry reg;
    EntityLifecycle life(reg, 63);
    const entt::entity original = life.create("original");
    const EntityRef original_ref = life.refOf(original);
    g_destroy_in_handler = &reg;
    g_handler_entity = original;
    life.destroy(original);
    if (life.pendingCount() != 1)
        return fail(name, "could not queue the original entity");
    life.destroyImmediate(original);
    const EntityRef replacement_ref{EntityGuid{0x6101, 0x6102}};
    if (g_destroyed_count != 1 || reg.valid(original) || life.resolve(original_ref) != entt::null ||
        life.pendingCount() != 0 || g_handler_replacement == entt::null ||
        !reg.valid(g_handler_replacement) || life.resolve(replacement_ref) != g_handler_replacement)
        return fail(name, "immediate handler retirement or replacement lifetime was incorrect");
    life.drainDestroyQueue();
    if (g_destroyed_count != 1 || !reg.valid(g_handler_replacement) ||
        life.resolve(replacement_ref) != g_handler_replacement)
        return fail(name, "drain repeated the event or destroyed the replacement");
    resetBus();
    return pass(name);
}

// L18: publication guards the entity across callbacks and lifecycle instances.
bool testL18()
{
    const std::string name = "L18 same-entity reentrant destruction is ignored";
    for (bool immediate : {false, true})
    {
        for (bool clear_first : {false, true})
        {
            resetBus();
            EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndReenter>();
            entt::registry reg;
            EntityLifecycle life(reg, 71);
            EntityLifecycle peer(reg, 72);
            const entt::entity entity = life.create("guarded");
            const EntityRef ref = life.refOf(entity);
            g_reentrant_lifecycle = &peer;
            g_reentrant_entity = entity;
            g_reentrant_clear = clear_first;
            life.destroy(entity);
            if (immediate)
                life.destroyImmediate(entity);
            else
                life.drainDestroyQueue();
            if (g_destroyed_count != 1 || reg.valid(entity) || life.resolve(ref) != entt::null ||
                peer.resolve(ref) != entt::null || life.pendingCount() != 0 || peer.pendingCount() != 0)
                return fail(name, "reentrant callback repeated publication or failed to retire the entity");
            life.drainDestroyQueue();
            peer.drainDestroyQueue();
            if (g_destroyed_count != 1)
                return fail(name, "later draining repeated the destruction event");
        }
    }
    resetBus();
    return pass(name);
}
// L19: a throwing observer cannot strand the drain or lose queued work.
bool testL19()
{
    const std::string name = "L19 interrupted drain resumes in FIFO order";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndThrowOnce>();
    entt::registry reg;
    EntityLifecycle life(reg, 81);
    const entt::entity first = life.create("first");
    const entt::entity second = life.create("second");
    const entt::entity third = life.create("third");
    const auto first_guid = life.refOf(first).guid;
    const auto second_guid = life.refOf(second).guid;
    const auto third_guid = life.refOf(third).guid;
    life.destroy(first);
    life.destroy(second);
    g_throw_lifecycle = &life;
    g_throw_enqueue = third;
    bool caught = false;
    try { life.drainDestroyQueue(); }
    catch (const std::runtime_error&) { caught = true; }
    if (!caught || life.pendingCount() != 3 || !reg.valid(first) ||
        !reg.valid(second) || !reg.valid(third))
        return fail(name, "exception did not preserve pending work");
    life.drainDestroyQueue();
    if (life.pendingCount() != 0 || reg.valid(first) || reg.valid(second) || reg.valid(third) ||
        g_throw_events != std::vector<EntityGuid>{first_guid, second_guid, third_guid})
        return fail(name, "retry lost FIFO order or repeated publication");
    resetBus();
    return pass(name);
}
// L20: immediate and deferred retries share publication state across services.
bool testL20()
{
    const std::string name = "L20 interrupted publication is shared across services";
    for (bool immediate_first : {false, true})
    {
        resetBus();
        EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedAndThrowOnce>();
        entt::registry reg;
        EntityLifecycle life(reg, 91);
        EntityLifecycle peer(reg, 92);
        const entt::entity entity = life.create("interrupted");
        const auto guid = life.refOf(entity).guid;
        if (!immediate_first)
            life.destroy(entity);
        g_throw_lifecycle = &life;
        bool caught = false;
        try
        {
            if (immediate_first) life.destroyImmediate(entity);
            else life.drainDestroyQueue();
        }
        catch (const std::runtime_error&) { caught = true; }
        if (!caught || !reg.valid(entity) || life.pendingCount() != 1)
            return fail(name, "failed operation did not retain the entity for retry");
        if (immediate_first)
            life.drainDestroyQueue();
        else
        {
            peer.destroyImmediate(entity);
            life.drainDestroyQueue();
        }
        if (reg.valid(entity) || life.pendingCount() != 0 ||
            g_throw_events != std::vector<EntityGuid>{guid})
            return fail(name, "retry republished or failed to retire the entity");
    }
    resetBus();
    return pass(name);
}
// L21: clear cancels later requests, even when its observer then throws.
bool testL21()
{
    const std::string name = "L21 clear and throw preserves published entity";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedClearAndThrow>();
    entt::registry reg;
    EntityLifecycle life(reg, 101);
    EntityLifecycle peer(reg, 102);
    const entt::entity first = life.create("first");
    const entt::entity second = life.create("second");
    const auto first_guid = life.refOf(first).guid;
    life.destroy(first);
    life.destroy(second);
    g_clear_on_destroy = &peer;
    bool caught = false;
    try { life.drainDestroyQueue(); }
    catch (const std::runtime_error&) { caught = true; }
    if (!caught || !reg.valid(first) || !reg.valid(second) ||
        life.pendingCount() != 1 || !life.isPendingDestroy(first) || life.isPendingDestroy(second))
        return fail(name, "clear did not retain only the published request");
    life.drainDestroyQueue();
    if (reg.valid(first) || !reg.valid(second) || life.pendingCount() != 0 ||
        g_throw_events != std::vector<EntityGuid>{first_guid})
        return fail(name, "retry repeated publication or failed to preserve the canceled entity");
    resetBus();
    return pass(name);
}
// L22: a peer drain cannot consume a request being destroyed elsewhere.
bool testL22()
{
    const std::string name = "L22 peer drain retains in-progress request";
    resetBus();
    EventBus::get().sink<EntityDestroyedEvent>().connect<&onDestroyedDrainPeerAndThrow>();
    entt::registry reg;
    EntityLifecycle life(reg, 111);
    EntityLifecycle peer(reg, 112);
    const entt::entity first = life.create("first");
    const entt::entity second = life.create("second");
    const auto first_guid = life.refOf(first).guid;
    const auto second_guid = life.refOf(second).guid;
    peer.destroy(first);
    peer.destroy(second);
    g_drain_peer_on_destroy = &peer;
    bool caught = false;
    try { life.destroyImmediate(first); }
    catch (const std::runtime_error&) { caught = true; }
    if (!caught || !reg.valid(first) || !reg.valid(second) || peer.pendingCount() != 2 ||
        !peer.isPendingDestroy(first) || !peer.isPendingDestroy(second))
        return fail(name, "peer drain consumed or reordered blocked work");
    peer.drainDestroyQueue();
    if (reg.valid(first) || reg.valid(second) || peer.pendingCount() != 0 ||
        g_throw_events != std::vector<EntityGuid>{first_guid, second_guid})
        return fail(name, "peer retry lost work, FIFO order, or repeated publication");
    resetBus();
    return pass(name);
}
} // namespace

int main()
{
    int failures = 0;
    if (!testL1()) ++failures;
    if (!testL2()) ++failures;
    if (!testL3()) ++failures;
    if (!testL4()) ++failures;
    if (!testL5()) ++failures;
    if (!testL6()) ++failures;
    if (!testL7()) ++failures;
    if (!testL8()) ++failures;
    if (!testL9()) ++failures;
    if (!testL10()) ++failures;
    if (!testL11()) ++failures;
    if (!testL12()) ++failures;
    if (!testL13()) ++failures;
    if (!testL14()) ++failures;
    if (!testL15()) ++failures;
    if (!testL16()) ++failures;
    if (!testL17()) ++failures;
    if (!testL18()) ++failures;
    if (!testL19()) ++failures;
    if (!testL20()) ++failures;
    if (!testL21()) ++failures;
    if (!testL22()) ++failures;
    if (failures == 0)
        std::cout << "All EntityLifecycleTests passed" << std::endl;
    else
        std::cerr << failures << " EntityLifecycleTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
