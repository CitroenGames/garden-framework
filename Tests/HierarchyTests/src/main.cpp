#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/EntityLifecycle.hpp"
#include "ECS/Hierarchy.hpp"
#include "Events/EventBus.hpp"

#include <cmath>
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
bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bool nearMat(const glm::mat4& a, const glm::mat4& b, float eps = 1e-5f)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!near(a[c][r], b[c][r], eps)) return false;
    return true;
}

entt::entity makeEntity(entt::registry& reg, EntityLifecycle& life,
                        const glm::vec3& pos)
{
    entt::entity e = life.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.position = pos;
    t.rotation = glm::vec3(0.0f);
    t.scale = glm::vec3(1.0f);
    return e;
}

glm::vec3 worldPos(entt::registry& reg, entt::entity e)
{
    return reg.get<WorldTransformComponent>(e).position;
}

// H1: attach + propagate, nested 3-level (doc hierarchy.md H1)
bool testAttachPropagate()
{
    const std::string name = "H1 attach+propagate nested 3-level";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 42);
    entt::entity root = makeEntity(reg, life, {10, 0, 0});
    entt::entity child = makeEntity(reg, life, {1, 0, 0});
    entt::entity grand = makeEntity(reg, life, {0, 2, 0});
    Hierarchy::attach(reg, child, root);
    Hierarchy::attach(reg, grand, child);
    if (Hierarchy::parentOf(reg, child) != root) return fail(name, "parentOf(child)!=root");
    if (Hierarchy::parentOf(reg, grand) != child) return fail(name, "parentOf(grand)!=child");
    if (Hierarchy::parentOf(reg, root) != entt::null) return fail(name, "root has parent");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, root), {10, 0, 0})) return fail(name, "root world != (10,0,0)");
    if (!nearVec(worldPos(reg, child), {11, 0, 0})) return fail(name, "child world != (11,0,0)");
    if (!nearVec(worldPos(reg, grand), {11, 2, 0})) return fail(name, "grandchild world != (11,2,0)");
    if (reg.get<WorldTransformComponent>(grand).dirty) return fail(name, "dirty not cleared");
    if (reg.all_of<HierarchyDirtyTag>(grand)) return fail(name, "dirty tag not removed");
    return pass(name);
}

// H2: rotated parent (doc H2)
bool testRotation()
{
    const std::string name = "H2 rotated parent propagation";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 43);
    entt::entity root = makeEntity(reg, life, {0, 0, 0});
    reg.get<TransformComponent>(root).rotation = {0, 90, 0};
    entt::entity child = makeEntity(reg, life, {1, 0, 0});
    Hierarchy::attach(reg, child, root);
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), {0, 0, -1})) return fail(name, "child world != (0,0,-1)");
    return pass(name);
}

// H3: keep_world attach (doc H3)
bool testKeepWorldAttach()
{
    const std::string name = "H3 keep_world attach preserves world";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 44);
    entt::entity root = makeEntity(reg, life, {10, 0, 0});
    entt::entity child = makeEntity(reg, life, {11, 0, 0});
    Hierarchy::updateTransformPropagation(reg); // no hierarchy yet: skipped
    const glm::mat4 before = Hierarchy::computeWorldMatrix(reg, child);
    Hierarchy::attach(reg, child, root, true);
    const auto& local = reg.get<TransformComponent>(child);
    if (!nearVec(local.position, {1, 0, 0})) return fail(name, "local not rebased to (1,0,0)");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearMat(reg.get<WorldTransformComponent>(child).matrix, before))
        return fail(name, "world matrix changed after keep_world attach");
    if (!nearVec(worldPos(reg, child), {11, 0, 0})) return fail(name, "world pos changed");
    return pass(name);
}

// H4: cycle rejected (doc H4)
bool testCycleRejected()
{
    const std::string name = "H4 cycle attach rejected";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 45);
    entt::entity a = makeEntity(reg, life, {0, 0, 0});
    entt::entity b = makeEntity(reg, life, {1, 0, 0});
    Hierarchy::attach(reg, b, a);
    Hierarchy::attach(reg, a, b); // would create A->B->A
    if (Hierarchy::parentOf(reg, a) != entt::null) return fail(name, "cycle attach changed parentOf(A)");
    if (Hierarchy::parentOf(reg, b) != a) return fail(name, "hierarchy changed after rejected attach");
    Hierarchy::attach(reg, a, a);
    if (Hierarchy::parentOf(reg, a) != entt::null) return fail(name, "self attach not rejected");
    if (!Hierarchy::isDescendantOf(reg, b, a)) return fail(name, "isDescendantOf(B,A) false");
    if (Hierarchy::isDescendantOf(reg, a, b)) return fail(name, "isDescendantOf(A,B) true");
    return pass(name);
}

// detach (keep_world default true)
bool testDetach()
{
    const std::string name = "H-detach child becomes root, world preserved";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 46);
    entt::entity root = makeEntity(reg, life, {10, 0, 0});
    entt::entity child = makeEntity(reg, life, {1, 0, 0});
    Hierarchy::attach(reg, child, root);
    Hierarchy::updateTransformPropagation(reg);
    Hierarchy::detach(reg, child);
    if (Hierarchy::parentOf(reg, child) != entt::null) return fail(name, "still parented after detach");
    if (reg.all_of<ParentComponent>(child)) return fail(name, "ParentComponent remains");
    const auto* children = reg.try_get<ChildrenComponent>(root);
    if (children && !children->children.empty()) return fail(name, "root Children not emptied");
    if (!nearVec(reg.get<TransformComponent>(child).position, {11, 0, 0}))
        return fail(name, "local not rebased to world on detach");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), {11, 0, 0})) return fail(name, "world changed after detach");
    return pass(name);
}

// H6: cascade destroy (doc H6)
bool testCascadeDestroy()
{
    const std::string name = "H6 cascade destroy removes subtree only";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 47);
    entt::entity root = makeEntity(reg, life, {0, 0, 0});
    reg.emplace<HierarchyPolicyComponent>(root).on_parent_destroyed =
        HierarchyDestroyPolicy::CascadeDestroy;
    entt::entity child = makeEntity(reg, life, {1, 0, 0});
    entt::entity grand = makeEntity(reg, life, {0, 1, 0});
    entt::entity other = makeEntity(reg, life, {5, 5, 5});
    Hierarchy::attach(reg, child, root);
    Hierarchy::attach(reg, grand, child);
    life.destroy(root);
    life.drainDestroyQueue();
    if (reg.valid(root)) return fail(name, "root still valid");
    if (reg.valid(child)) return fail(name, "child still valid after cascade");
    if (reg.valid(grand)) return fail(name, "grandchild still valid after cascade");
    if (!reg.valid(other)) return fail(name, "unrelated entity destroyed");
    return pass(name);
}

// H5: orphan destroy, default policy (doc H5): child -> grandparent / root
bool testOrphanDestroy()
{
    const std::string name = "H5 orphan destroy reparents, world preserved";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 48);
    entt::entity gp = makeEntity(reg, life, {10, 0, 0});
    entt::entity parent = makeEntity(reg, life, {1, 0, 0});
    entt::entity child = makeEntity(reg, life, {0, 2, 0});
    Hierarchy::attach(reg, parent, gp);
    Hierarchy::attach(reg, child, parent);
    Hierarchy::updateTransformPropagation(reg);
    const glm::vec3 before = worldPos(reg, child); // (11,2,0)
    life.destroy(parent); // default OrphanToParent
    life.drainDestroyQueue();
    if (reg.valid(parent)) return fail(name, "parent still valid");
    if (!reg.valid(child)) return fail(name, "child destroyed under orphan policy");
    if (Hierarchy::parentOf(reg, child) != gp) return fail(name, "child not reparented to grandparent");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), before, 1e-5f)) return fail(name, "child world changed after orphan");
    // Root-parent variant: destroying a root orphans children to root.
    life.destroy(gp);
    life.drainDestroyQueue();
    if (!reg.valid(child)) return fail(name, "child destroyed when root parent died");
    if (Hierarchy::parentOf(reg, child) != entt::null) return fail(name, "child not orphaned to root");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), before, 1e-5f)) return fail(name, "child world changed after root orphan");
    return pass(name);
}

// ReparentToRoot policy (supplements H5)
bool testReparentToRoot()
{
    const std::string name = "H5b ReparentToRoot destroy policy";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 49);
    entt::entity gp = makeEntity(reg, life, {10, 0, 0});
    entt::entity parent = makeEntity(reg, life, {1, 0, 0});
    reg.emplace<HierarchyPolicyComponent>(parent).on_parent_destroyed =
        HierarchyDestroyPolicy::ReparentToRoot;
    entt::entity child = makeEntity(reg, life, {0, 2, 0});
    Hierarchy::attach(reg, parent, gp);
    Hierarchy::attach(reg, child, parent);
    Hierarchy::updateTransformPropagation(reg);
    life.destroy(parent);
    life.drainDestroyQueue();
    if (Hierarchy::parentOf(reg, child) != entt::null) return fail(name, "child not reparented to root");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), {11, 2, 0}, 1e-5f)) return fail(name, "child world changed");
    return pass(name);
}

// Stale parent resolve: parent destroyed immediately, Parent GUID dangling;
// rebuildChildren orphans the child to root and ParentComponent is removed.
// Also rebuildChildren rebuilds Children from Parent alone (serialization §5).
bool testStaleParentAndRebuild()
{
    const std::string name = "H-stale parent resolve + rebuildChildren";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 50);
    entt::entity root = makeEntity(reg, life, {10, 0, 0});
    entt::entity child = makeEntity(reg, life, {1, 0, 0});
    entt::entity grand = makeEntity(reg, life, {0, 2, 0});
    Hierarchy::attach(reg, child, root);
    Hierarchy::attach(reg, grand, child);
    // Simulate load: drop Children, rebuild from Parent only.
    reg.remove<ChildrenComponent>(root);
    reg.remove<ChildrenComponent>(child);
    Hierarchy::rebuildChildren(reg);
    const auto* rc = reg.try_get<ChildrenComponent>(root);
    if (!rc || rc->children.size() != 1) return fail(name, "rebuilt root Children wrong");
    if (Hierarchy::parentOf(reg, grand) != child) return fail(name, "parentOf broken after rebuild");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, grand), {11, 2, 0})) return fail(name, "world wrong after rebuild+propagate");
    // Stale parent: raw registry destroy bypasses lifecycle policy.
    EntityRef child_ref = life.refOf(child);
    reg.destroy(root);
    if (life.resolve(child_ref) != child) return fail(name, "child resolve broken");
    if (Hierarchy::parentOf(reg, child) != entt::null)
        return fail(name, "stale parent did not resolve to null");
    Hierarchy::rebuildChildren(reg); // must orphan child to root
    if (reg.all_of<ParentComponent>(child)) return fail(name, "orphan ParentComponent not removed");
    if (Hierarchy::parentOf(reg, child) != entt::null) return fail(name, "orphan not root after rebuild");
    Hierarchy::updateTransformPropagation(reg);
    if (!reg.valid(grand)) return fail(name, "grandchild lost in stale-parent rebuild");
    return pass(name);
}

// Depth limit 64 failure case: chain of 70 still terminates; node beyond
// limit is not propagated (no crash, no hang).
bool testDepthLimit()
{
    const std::string name = "H-depth limit 64 terminates";
    EventBus::get().clear();
    entt::registry reg;
    EntityLifecycle life(reg, 51);
    std::vector<entt::entity> chain;
    chain.push_back(makeEntity(reg, life, {0, 0, 0}));
    for (int i = 1; i < 70; ++i)
    {
        chain.push_back(makeEntity(reg, life, {1, 0, 0}));
        Hierarchy::attach(reg, chain.back(), chain[chain.size() - 2]);
    }
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, chain[10]), {10, 0, 0})) return fail(name, "depth-10 world wrong");
    // markDirty on a deep chain must also terminate (cycle-safe walk).
    Hierarchy::markDirty(reg, chain[0]);
    Hierarchy::updateTransformPropagation(reg);
    return pass(name);
}
} // namespace

int main()
{
    int failures = 0;
    if (!testAttachPropagate()) ++failures;
    if (!testRotation()) ++failures;
    if (!testKeepWorldAttach()) ++failures;
    if (!testCycleRejected()) ++failures;
    if (!testDetach()) ++failures;
    if (!testCascadeDestroy()) ++failures;
    if (!testOrphanDestroy()) ++failures;
    if (!testReparentToRoot()) ++failures;
    if (!testStaleParentAndRebuild()) ++failures;
    if (!testDepthLimit()) ++failures;
    if (failures == 0)
        std::cout << "All HierarchyTests passed" << std::endl;
    else
        std::cerr << failures << " HierarchyTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
