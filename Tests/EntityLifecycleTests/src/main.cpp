#include "Components/IdentityComponent.hpp"
#include "Components/Components.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <iostream>
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
entt::entity g_destroyed_entity = entt::null;

void onCreated(const EntityCreatedEvent&) { ++g_created_count; }
void onDestroyed(const EntityDestroyedEvent& e)
{
    ++g_destroyed_count;
    g_last_destroyed_guid = e.ref.guid;
    g_destroyed_entity = e.entity;
    if (g_observe_registry)
        g_destroyed_seen_valid = g_observe_registry->valid(e.entity);
}

void resetBus()
{
    EventBus::get().sink<EntityCreatedEvent>().disconnect<&onCreated>();
    EventBus::get().sink<EntityDestroyedEvent>().disconnect<&onDestroyed>();
    EventBus::get().clear();
    g_created_count = 0;
    g_destroyed_count = 0;
    g_destroyed_seen_valid = false;
    g_last_destroyed_guid = EntityGuid{};
    g_destroyed_entity = entt::null;
    g_observe_registry = nullptr;
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
    if (failures == 0)
        std::cout << "All EntityLifecycleTests passed" << std::endl;
    else
        std::cerr << failures << " EntityLifecycleTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
