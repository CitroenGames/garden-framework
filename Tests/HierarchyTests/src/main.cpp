#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "world.hpp"
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
bool resolvesTo(entt::registry& reg, entt::entity entity,
                const glm::mat4& expected)
{
    const auto resolved = Hierarchy::resolveRenderWorldMatrix(reg, entity);
    return resolved.has_value() && nearMat(*resolved, expected);
}

entt::entity makeEntity(entt::registry& reg, world& owner,
                        const glm::vec3& pos)
{
    entt::entity e = resolveEntityRef(reg, owner.persistedReference(owner.createScopedEntity()));
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

bool hasChildRef(const ChildrenComponent& children, EntityRef child)
{
    for (const EntityRef& ref : children.children)
        if (ref == child) return true;
    return false;
}

bool testRenderWorldPoseResolver()
{
    const std::string name = "H-D render world pose resolves presentation hierarchy without mutation";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;

    const entt::entity root = makeEntity(reg, owner, {10, 0, 0});
    const entt::entity child = makeEntity(reg, owner, {1, 2, 0});
    const entt::entity leaf = makeEntity(reg, owner, {0, 0, 2});
    auto& root_transform = reg.get<TransformComponent>(root);
    root_transform.rotation = {0, 20, 0};
    root_transform.scale = {1.5f, 1.0f, 1.0f};
    auto& child_transform = reg.get<TransformComponent>(child);
    child_transform.rotation = {0, 0, 10};
    child_transform.scale = {2.0f, 3.0f, 1.0f};
    auto& leaf_transform = reg.get<TransformComponent>(leaf);
    leaf_transform.rotation = {5, 0, 0};
    leaf_transform.scale = {0.5f, 0.75f, 1.25f};

    const PresentationTransformComponent root_presentation{{20, 0, 0}, {0, 45, 0}};
    const PresentationTransformComponent child_presentation{{3, 1, 0}, {0, 0, 35}};
    reg.emplace<PresentationTransformComponent>(root, root_presentation);
    reg.emplace<PresentationTransformComponent>(child, child_presentation);
    reg.emplace<ParentComponent>(child, ParentComponent{EntityRef{reg.get<IdentityComponent>(root).guid}});
    reg.emplace<ParentComponent>(leaf, ParentComponent{EntityRef{reg.get<IdentityComponent>(child).guid}});

    TransformComponent expected_root = root_transform;
    expected_root.position = root_presentation.position;
    expected_root.rotation = root_presentation.rotation;
    TransformComponent expected_child = child_transform;
    expected_child.position = child_presentation.position;
    expected_child.rotation = child_presentation.rotation;
    const glm::mat4 expected_root_matrix = expected_root.getTransformMatrix();
    const glm::mat4 expected_child_matrix = expected_root_matrix *
                                            expected_child.getTransformMatrix();
    const glm::mat4 expected_leaf_matrix = expected_child_matrix *
                                           leaf_transform.getTransformMatrix();

    const TransformComponent root_before = root_transform;
    const TransformComponent child_before = child_transform;
    const TransformComponent leaf_before = leaf_transform;
    const PresentationTransformComponent root_presentation_before =
        reg.get<PresentationTransformComponent>(root);
    const PresentationTransformComponent child_presentation_before =
        reg.get<PresentationTransformComponent>(child);
    const ParentComponent child_parent_before = reg.get<ParentComponent>(child);
    const ParentComponent leaf_parent_before = reg.get<ParentComponent>(leaf);

    if (!resolvesTo(reg, root, expected_root_matrix))
        return fail(name, "root presentation pose was not resolved as a world matrix");
    if (!resolvesTo(reg, child, expected_child_matrix))
        return fail(name, "child did not compose parent and child presentation poses");
    if (!resolvesTo(reg, leaf, expected_leaf_matrix))
        return fail(name, "multilevel pose did not inherit presentation from ancestors");

    if (!nearVec(root_transform.position, root_before.position) ||
        !nearVec(root_transform.rotation, root_before.rotation) ||
        !nearVec(root_transform.scale, root_before.scale) ||
        !nearVec(child_transform.position, child_before.position) ||
        !nearVec(child_transform.rotation, child_before.rotation) ||
        !nearVec(child_transform.scale, child_before.scale) ||
        !nearVec(leaf_transform.position, leaf_before.position) ||
        !nearVec(leaf_transform.rotation, leaf_before.rotation) ||
        !nearVec(leaf_transform.scale, leaf_before.scale))
        return fail(name, "resolver mutated an authoritative local transform");
    if (!nearVec(reg.get<PresentationTransformComponent>(root).position,
                 root_presentation_before.position) ||
        !nearVec(reg.get<PresentationTransformComponent>(root).rotation,
                 root_presentation_before.rotation) ||
        !nearVec(reg.get<PresentationTransformComponent>(child).position,
                 child_presentation_before.position) ||
        !nearVec(reg.get<PresentationTransformComponent>(child).rotation,
                 child_presentation_before.rotation))
        return fail(name, "resolver mutated presentation input");
    if (reg.get<ParentComponent>(child).parent != child_parent_before.parent ||
        reg.get<ParentComponent>(leaf).parent != leaf_parent_before.parent)
        return fail(name, "resolver mutated hierarchy links");
    for (entt::entity entity : {root, child, leaf})
        if (reg.any_of<WorldTransformComponent, HierarchyDirtyTag>(entity))
            return fail(name, "resolver wrote or created derived hierarchy state");

    // Parent scale multiplied by child rotation produces a sheared full matrix.
    const entt::entity scaled_root = makeEntity(reg, owner, {0, 0, 0});
    const entt::entity rotated_child = makeEntity(reg, owner, {0, 0, 0});
    reg.get<TransformComponent>(scaled_root).scale = {2, 1, 1};
    reg.get<TransformComponent>(rotated_child).rotation = {0, 0, 45};
    reg.emplace<ParentComponent>(rotated_child,
                                 ParentComponent{EntityRef{reg.get<IdentityComponent>(scaled_root).guid}});
    const glm::mat4 expected_sheared =
        reg.get<TransformComponent>(scaled_root).getTransformMatrix() *
        reg.get<TransformComponent>(rotated_child).getTransformMatrix();
    const auto resolved_sheared =
        Hierarchy::resolveRenderWorldMatrix(reg, rotated_child);
    if (!resolved_sheared)
        return fail(name, "valid scale/rotation hierarchy did not resolve");
    const glm::vec3 basis_x((*resolved_sheared)[0]);
    const glm::vec3 basis_y((*resolved_sheared)[1]);
    if (!nearMat(*resolved_sheared, expected_sheared) ||
        std::abs(glm::dot(basis_x, basis_y)) < 1e-3f)
        return fail(name, "inherited scale/shear was not preserved in the full matrix");

    return pass(name);
}

bool testRenderWorldPoseInvalidParents()
{
    const std::string name = "H-D unresolved render parents fall back to local pose";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;

    const entt::entity stale_child = makeEntity(reg, owner, {2, 3, 4});
    reg.get<TransformComponent>(stale_child).scale = {2, 2, 2};
    const PresentationTransformComponent presentation{{8, 9, 10}, {0, 0, 25}};
    reg.emplace<PresentationTransformComponent>(stale_child, presentation);
    const EntityRef stale_ref{EntityGuid{0xdead, 0xbeef}};
    reg.emplace<ParentComponent>(stale_child, ParentComponent{stale_ref});
    TransformComponent expected = reg.get<TransformComponent>(stale_child);
    expected.position = presentation.position;
    expected.rotation = presentation.rotation;
    if (!resolvesTo(reg, stale_child, expected.getTransformMatrix()))
        return fail(name, "stale parent did not resolve as a root at its presented local pose");
    if (reg.get<ParentComponent>(stale_child).parent != stale_ref)
        return fail(name, "stale parent fallback mutated its authored link");

    const entt::entity invalid_ref_child = makeEntity(reg, owner, {5, 6, 7});
    reg.emplace<ParentComponent>(invalid_ref_child,
        ParentComponent{EntityRef{EntityGuid{0, 42}}});
    if (!resolvesTo(reg, invalid_ref_child,
                    reg.get<TransformComponent>(invalid_ref_child).getTransformMatrix()))
        return fail(name, "unresolvable identity ref did not use local pose");

    const EntityGuid duplicate_guid{0xabc, 0xdef};
    entt::registry ambiguous_reg;
    const entt::entity duplicate_parent_a = ambiguous_reg.create();
    const entt::entity duplicate_parent_b = ambiguous_reg.create();
    ambiguous_reg.emplace<IdentityComponent>(duplicate_parent_a,
                                             IdentityComponent{duplicate_guid});
    ambiguous_reg.emplace<IdentityComponent>(duplicate_parent_b,
                                             IdentityComponent{duplicate_guid});
    ambiguous_reg.emplace<TransformComponent>(duplicate_parent_a, 100, 0, 0);
    ambiguous_reg.emplace<TransformComponent>(duplicate_parent_b, 200, 0, 0);
    const entt::entity ambiguous_ref_child = ambiguous_reg.create();
    ambiguous_reg.emplace<IdentityComponent>(ambiguous_ref_child,
                                             IdentityComponent{EntityGuid{1, 1}});
    ambiguous_reg.emplace<TransformComponent>(ambiguous_ref_child, 7, 8, 9);
    ambiguous_reg.emplace<ParentComponent>(ambiguous_ref_child,
                                           ParentComponent{EntityRef{duplicate_guid}});
    if (!resolvesTo(ambiguous_reg, ambiguous_ref_child,
                    ambiguous_reg.get<TransformComponent>(ambiguous_ref_child).getTransformMatrix()))
        return fail(name, "ambiguous parent identity did not use local pose");

    const entt::entity null_parent_child = makeEntity(reg, owner, {4, 3, 2});
    reg.emplace<ParentComponent>(null_parent_child, ParentComponent{EntityRef{}});
    if (!resolvesTo(reg, null_parent_child,
                    reg.get<TransformComponent>(null_parent_child).getTransformMatrix()))
        return fail(name, "null parent ref did not resolve as a root");

    if (Hierarchy::resolveRenderWorldMatrix(reg, entt::null).has_value())
        return fail(name, "invalid requested entity unexpectedly resolved a pose");
    for (entt::entity entity : {stale_child, invalid_ref_child, null_parent_child})
        if (reg.any_of<WorldTransformComponent, HierarchyDirtyTag>(entity))
            return fail(name, "fallback mutated derived hierarchy state");
    return pass(name);
}

bool testRenderWorldPoseCycle()
{
    const std::string name = "H-D cyclic render links fail without a query-dependent pose";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    const entt::entity a = makeEntity(reg, owner, {10, 0, 0});
    const entt::entity b = makeEntity(reg, owner, {0, 20, 0});
    const entt::entity descendant = makeEntity(reg, owner, {0, 0, 3});
    reg.emplace<ParentComponent>(a, ParentComponent{EntityRef{reg.get<IdentityComponent>(b).guid}});
    reg.emplace<ParentComponent>(b, ParentComponent{EntityRef{reg.get<IdentityComponent>(a).guid}});
    reg.emplace<ParentComponent>(descendant, ParentComponent{EntityRef{reg.get<IdentityComponent>(a).guid}});

    if (Hierarchy::resolveRenderWorldMatrix(reg, descendant).has_value())
        return fail(name, "descendant traversal through a cyclic graph returned a pose");
    if (Hierarchy::resolveRenderWorldMatrix(reg, a).has_value())
        return fail(name, "direct cyclic traversal returned a pose");
    return pass(name);
}

template<typename Configure>
bool rejectedIdentityAttachLeavesFreshEntitiesUntouched(
    const std::string& name, Configure configure)
{
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    entt::entity child = makeEntity(reg, owner, {1, 2, 3});
    entt::entity parent = makeEntity(reg, owner, {4, 5, 6});
    entt::entity other = makeEntity(reg, owner, {7, 8, 9});
    const glm::mat4 child_transform = reg.get<TransformComponent>(child).getTransformMatrix();
    const glm::mat4 parent_transform = reg.get<TransformComponent>(parent).getTransformMatrix();
    const glm::mat4 other_transform = reg.get<TransformComponent>(other).getTransformMatrix();

    configure(reg, child, parent, other);
    if (Hierarchy::attach(reg, child, parent))
        return fail(name, "attach accepted invalid identity input");

    for (entt::entity e : {child, parent, other})
    {
        if (reg.any_of<ParentComponent, ChildrenComponent,
                       WorldTransformComponent, HierarchyDirtyTag>(e))
            return fail(name, "rejected attach changed hierarchy state");
    }
    if (!nearMat(reg.get<TransformComponent>(child).getTransformMatrix(), child_transform) ||
        !nearMat(reg.get<TransformComponent>(parent).getTransformMatrix(), parent_transform) ||
        !nearMat(reg.get<TransformComponent>(other).getTransformMatrix(), other_transform))
        return fail(name, "rejected attach changed a transform");
    return true;
}

bool testIdentityPreconditions()
{
    const std::string name = "H-A identity preconditions reject before mutation";
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A missing child identity", [](entt::registry& reg, entt::entity child,
                                               entt::entity, entt::entity) {
                reg.remove<IdentityComponent>(child);
            })) return false;
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A missing parent identity", [](entt::registry& reg, entt::entity,
                                               entt::entity parent, entt::entity) {
                reg.remove<IdentityComponent>(parent);
            })) return false;
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A null child identity GUID", [](entt::registry& reg, entt::entity child,
                                                entt::entity, entt::entity) {
                reg.get<IdentityComponent>(child).guid = {};
            })) return false;
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A null parent identity GUID", [](entt::registry& reg, entt::entity,
                                                entt::entity parent, entt::entity) {
                reg.get<IdentityComponent>(parent).guid = {};
            })) return false;
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A duplicate child identity GUID", [](entt::registry& reg, entt::entity child,
                                                     entt::entity, entt::entity other) {
                reg.get<IdentityComponent>(other).guid =
                    reg.get<IdentityComponent>(child).guid;
            })) return false;
    if (!rejectedIdentityAttachLeavesFreshEntitiesUntouched(
            "H-A duplicate parent identity GUID", [](entt::registry& reg, entt::entity,
                                                      entt::entity parent, entt::entity other) {
                reg.get<IdentityComponent>(other).guid =
                    reg.get<IdentityComponent>(parent).guid;
            })) return false;
    return pass(name);
}

bool testRejectedReattachPreservesOriginalLink()
{
    const std::string name = "H-A rejected reattach preserves original link and mirrors";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    entt::entity original_parent = makeEntity(reg, owner, {10, 0, 0});
    entt::entity new_parent = makeEntity(reg, owner, {20, 0, 0});
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
    entt::entity duplicate = makeEntity(reg, owner, {30, 0, 0});
    if (!Hierarchy::attach(reg, child, original_parent))
        return fail(name, "valid initial attach was rejected");

    const EntityRef original_parent_ref =
        EntityRef{reg.get<IdentityComponent>(original_parent).guid};
    const EntityRef child_ref{reg.get<IdentityComponent>(child).guid};
    const std::vector<EntityRef> original_children =
        reg.get<ChildrenComponent>(original_parent).children;
    reg.get<IdentityComponent>(duplicate).guid =
        reg.get<IdentityComponent>(new_parent).guid;

    if (Hierarchy::attach(reg, child, new_parent))
        return fail(name, "reattach accepted duplicated parent identity");
    const auto* parent = reg.try_get<ParentComponent>(child);
    if (!parent || parent->parent != original_parent_ref)
        return fail(name, "child no longer points to original parent");
    const auto* old_children = reg.try_get<ChildrenComponent>(original_parent);
    if (!old_children || old_children->children != original_children ||
        old_children->children.size() != 1 || !hasChildRef(*old_children, child_ref))
        return fail(name, "original parent's Children mirror changed");
    if (reg.all_of<ChildrenComponent>(new_parent) ||
        reg.all_of<WorldTransformComponent>(new_parent) ||
        reg.all_of<HierarchyDirtyTag>(new_parent))
        return fail(name, "rejected target parent was mutated");
    return pass(name);
}

// H1: attach + propagate, nested 3-level (doc hierarchy.md H1)
bool testAttachPropagate()
{
    const std::string name = "H1 attach+propagate nested 3-level";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {10, 0, 0});
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
    entt::entity grand = makeEntity(reg, owner, {0, 2, 0});
    if (!Hierarchy::attach(reg, child, root)) return fail(name, "valid root attach rejected");
    if (!Hierarchy::attach(reg, grand, child)) return fail(name, "valid nested attach rejected");
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
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {0, 0, 0});
    reg.get<TransformComponent>(root).rotation = {0, 90, 0};
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
    if (!Hierarchy::attach(reg, child, root)) return fail(name, "valid attach rejected");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), {0, 0, -1})) return fail(name, "child world != (0,0,-1)");
    return pass(name);
}

// H3: keep_world attach (doc H3)
bool testKeepWorldAttach()
{
    const std::string name = "H3 keep_world attach preserves world";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {10, 0, 0});
    entt::entity child = makeEntity(reg, owner, {11, 0, 0});
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
    world owner;
    auto& reg = owner.registry;
    entt::entity a = makeEntity(reg, owner, {0, 0, 0});
    entt::entity b = makeEntity(reg, owner, {1, 0, 0});
    if (!Hierarchy::attach(reg, b, a)) return fail(name, "valid initial attach rejected");
    if (Hierarchy::attach(reg, a, b)) return fail(name, "cycle attach reported success"); // would create A->B->A
    if (Hierarchy::parentOf(reg, a) != entt::null) return fail(name, "cycle attach changed parentOf(A)");
    if (Hierarchy::parentOf(reg, b) != a) return fail(name, "hierarchy changed after rejected attach");
    if (Hierarchy::attach(reg, a, a)) return fail(name, "self attach reported success");
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
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {10, 0, 0});
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
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
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {0, 0, 0});
    reg.emplace<HierarchyPolicyComponent>(root).on_parent_destroyed =
        HierarchyDestroyPolicy::CascadeDestroy;
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
    entt::entity grand = makeEntity(reg, owner, {0, 1, 0});
    entt::entity other = makeEntity(reg, owner, {5, 5, 5});
    Hierarchy::attach(reg, child, root);
    Hierarchy::attach(reg, grand, child);
    owner.requestRetirement(owner.scopedEntity(EntityRef{reg.get<IdentityComponent>(root).guid}));
    owner.drainRetirements();
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
    world owner;
    auto& reg = owner.registry;
    entt::entity gp = makeEntity(reg, owner, {10, 0, 0});
    entt::entity parent = makeEntity(reg, owner, {1, 0, 0});
    entt::entity child = makeEntity(reg, owner, {0, 2, 0});
    Hierarchy::attach(reg, parent, gp);
    Hierarchy::attach(reg, child, parent);
    Hierarchy::updateTransformPropagation(reg);
    const glm::vec3 before = worldPos(reg, child); // (11,2,0)
    owner.requestRetirement(owner.scopedEntity(EntityRef{reg.get<IdentityComponent>(parent).guid})); // default OrphanToParent
    owner.drainRetirements();
    if (reg.valid(parent)) return fail(name, "parent still valid");
    if (!reg.valid(child)) return fail(name, "child destroyed under orphan policy");
    if (Hierarchy::parentOf(reg, child) != gp) return fail(name, "child not reparented to grandparent");
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, child), before, 1e-5f)) return fail(name, "child world changed after orphan");
    // Root-parent variant: destroying a root orphans children to root.
    owner.requestRetirement(owner.scopedEntity(EntityRef{reg.get<IdentityComponent>(gp).guid}));
    owner.drainRetirements();
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
    world owner;
    auto& reg = owner.registry;
    entt::entity gp = makeEntity(reg, owner, {10, 0, 0});
    entt::entity parent = makeEntity(reg, owner, {1, 0, 0});
    reg.emplace<HierarchyPolicyComponent>(parent).on_parent_destroyed =
        HierarchyDestroyPolicy::ReparentToRoot;
    entt::entity child = makeEntity(reg, owner, {0, 2, 0});
    Hierarchy::attach(reg, parent, gp);
    Hierarchy::attach(reg, child, parent);
    Hierarchy::updateTransformPropagation(reg);
    owner.requestRetirement(owner.scopedEntity(EntityRef{reg.get<IdentityComponent>(parent).guid}));
    owner.drainRetirements();
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
    world owner;
    auto& reg = owner.registry;
    entt::entity root = makeEntity(reg, owner, {10, 0, 0});
    entt::entity child = makeEntity(reg, owner, {1, 0, 0});
    entt::entity grand = makeEntity(reg, owner, {0, 2, 0});
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
    EntityRef child_ref = EntityRef{reg.get<IdentityComponent>(child).guid};
    reg.destroy(root);
    if (resolveEntityRef(reg, child_ref) != child) return fail(name, "child resolve broken");
    if (Hierarchy::parentOf(reg, child) != entt::null)
        return fail(name, "stale parent did not resolve to null");
    Hierarchy::rebuildChildren(reg); // must orphan child to root
    if (reg.all_of<ParentComponent>(child)) return fail(name, "orphan ParentComponent not removed");
    if (Hierarchy::parentOf(reg, child) != entt::null) return fail(name, "orphan not root after rebuild");
    Hierarchy::updateTransformPropagation(reg);
    if (!reg.valid(grand)) return fail(name, "grandchild lost in stale-parent rebuild");
    return pass(name);
}

// H-B helper coverage: ordered rebuild is isolated to one level-load cohort.
bool testCohortRebuildPreservesUnrelatedMirrors()
{
    const std::string name = "H-B cohort rebuild preserves unrelated mirrors and file order";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    const entt::entity outside_parent = makeEntity(reg, owner, {0, 0, 0});
    const entt::entity outside_child = makeEntity(reg, owner, {0, 0, 0});
    if (!Hierarchy::attach(reg, outside_child, outside_parent))
        return fail(name, "failed to build unrelated hierarchy");
    const auto outside_mirror = reg.get<ChildrenComponent>(outside_parent).children;

    const entt::entity root = makeEntity(reg, owner, {0, 0, 0});
    const entt::entity first_child = makeEntity(reg, owner, {0, 0, 0});
    const entt::entity second_child = makeEntity(reg, owner, {0, 0, 0});
    const entt::entity external_parent_child = makeEntity(reg, owner, {0, 0, 0});
    const EntityRef root_ref = EntityRef{reg.get<IdentityComponent>(root).guid};
    const EntityRef first_ref = EntityRef{reg.get<IdentityComponent>(first_child).guid};
    const EntityRef second_ref = EntityRef{reg.get<IdentityComponent>(second_child).guid};

    reg.emplace<ParentComponent>(first_child, ParentComponent{root_ref});
    reg.emplace<ParentComponent>(second_child, ParentComponent{root_ref});
    reg.emplace<ParentComponent>(external_parent_child,
                                 ParentComponent{EntityRef{reg.get<IdentityComponent>(outside_parent).guid}});
    reg.emplace<ChildrenComponent>(root, ChildrenComponent{{EntityRef{reg.get<IdentityComponent>(outside_child).guid}}});
    reg.emplace<ChildrenComponent>(first_child, ChildrenComponent{{second_ref}});

    Hierarchy::rebuildChildren(reg,
        {root, first_child, second_child, external_parent_child});

    const auto* rebuilt = reg.try_get<ChildrenComponent>(root);
    if (!rebuilt || rebuilt->children.size() != 2 ||
        rebuilt->children[0] != first_ref || rebuilt->children[1] != second_ref)
        return fail(name, "cohort Children mirror did not follow file order");
    if (reg.get<ChildrenComponent>(outside_parent).children != outside_mirror)
        return fail(name, "unrelated registry mirror changed");
    if (reg.all_of<ParentComponent>(external_parent_child))
        return fail(name, "external parent reference was not removed");
    return pass(name);
}

// Depth limit 64 failure case: chain of 70 still terminates; node beyond
// limit is not propagated (no crash, no hang).
bool testDepthLimit()
{
    const std::string name = "H-depth limit 64 terminates";
    EventBus::get().clear();
    world owner;
    auto& reg = owner.registry;
    std::vector<entt::entity> chain;
    chain.push_back(makeEntity(reg, owner, {0, 0, 0}));
    for (int i = 1; i < 70; ++i)
    {
        chain.push_back(makeEntity(reg, owner, {1, 0, 0}));
        Hierarchy::attach(reg, chain.back(), chain[chain.size() - 2]);
    }
    Hierarchy::updateTransformPropagation(reg);
    if (!nearVec(worldPos(reg, chain[10]), {10, 0, 0})) return fail(name, "depth-10 world wrong");
    // markDirty on a deep chain must also terminate (cycle-safe walk).
    Hierarchy::markDirty(reg, chain[0]);
    Hierarchy::updateTransformPropagation(reg);
    return pass(name);
}

// H-E portion 1: physics ownership is an attach-time hierarchy policy.  These
// cases deliberately use Hierarchy::attach rather than the validator helper so
// they prove callers cannot write an invalid link/mirror through the public
// hierarchy API.
bool testPhysicsAttachmentPolicyRejectsBeforeMutation()
{
    const std::string name = "H-E physics attachment policy rejects unsupported children before mutation";
    EventBus::get().clear();

    auto rejected_child = [&](const char* label, auto configure_child)
    {
        world owner;
        auto& reg = owner.registry;
        const entt::entity parent = makeEntity(reg, owner, {4, 0, 0});
        const entt::entity child = makeEntity(reg, owner, {1, 2, 3});
        configure_child(reg, child);

        const TransformComponent child_before = reg.get<TransformComponent>(child);
        if (Hierarchy::attach(reg, child, parent))
            return fail(name, std::string(label) + " child attach was accepted");
        if (reg.any_of<ParentComponent, ChildrenComponent,
                       WorldTransformComponent, HierarchyDirtyTag>(child))
            return fail(name, std::string(label) + " rejection mutated child hierarchy state");
        if (reg.any_of<ChildrenComponent>(parent))
            return fail(name, std::string(label) + " rejection created parent mirror state");
        const TransformComponent& child_after = reg.get<TransformComponent>(child);
        if (!nearVec(child_after.position, child_before.position) ||
            !nearVec(child_after.rotation, child_before.rotation) ||
            !nearVec(child_after.scale, child_before.scale))
            return fail(name, std::string(label) + " rejection changed the local transform");
        return true;
    };

    if (!rejected_child("dynamic", [](entt::registry& reg, entt::entity child) {
            auto& body = reg.emplace<RigidBodyComponent>(child);
            body.motion_type = BodyMotionType::Dynamic;
        }))
        return false;
    if (!rejected_child("static", [](entt::registry& reg, entt::entity child) {
            auto& body = reg.emplace<RigidBodyComponent>(child);
            body.motion_type = BodyMotionType::Static;
        }))
        return false;
    if (!rejected_child("CharacterVirtual", [](entt::registry& reg, entt::entity child) {
            reg.emplace<CharacterControllerComponent>(child);
        }))
        return false;

    // The invalid scale is on an ancestor rather than the immediate parent.
    // This catches a validator that checks only the direct parent.
    {
        world owner;
        auto& reg = owner.registry;
        const entt::entity grandparent = makeEntity(reg, owner, {0, 0, 0});
        const entt::entity parent = makeEntity(reg, owner, {1, 0, 0});
        const entt::entity child = makeEntity(reg, owner, {0, 2, 0});
        if (!Hierarchy::attach(reg, parent, grandparent))
            return fail(name, "valid non-physics parent setup was rejected");
        reg.get<TransformComponent>(grandparent).scale = {2.0f, 1.0f, 1.0f};
        auto& kinematic = reg.emplace<RigidBodyComponent>(child);
        kinematic.motion_type = BodyMotionType::Kinematic;
        kinematic.apply_gravity = false;

        const TransformComponent child_before = reg.get<TransformComponent>(child);
        const auto grandparent_children_before = reg.get<ChildrenComponent>(grandparent).children;
        if (Hierarchy::attach(reg, child, parent))
            return fail(name, "scaled-ancestor kinematic child attach was accepted");
        if (reg.any_of<ParentComponent, ChildrenComponent,
                       WorldTransformComponent, HierarchyDirtyTag>(child))
            return fail(name, "scaled-ancestor rejection mutated child hierarchy state");
        if (reg.get<ChildrenComponent>(grandparent).children != grandparent_children_before)
            return fail(name, "scaled-ancestor rejection changed the existing grandparent mirror");
        const TransformComponent& child_after = reg.get<TransformComponent>(child);
        if (!nearVec(child_after.position, child_before.position) ||
            !nearVec(child_after.rotation, child_before.rotation) ||
            !nearVec(child_after.scale, child_before.scale))
            return fail(name, "scaled-ancestor rejection changed the local transform");
    }

    return pass(name);
}
bool testRetirementHierarchyFence()
{
    const std::string name = "L1 hierarchy retirement fence and policy";
    RetirementFence fence;
    world owner;
    auto& reg = owner.registry;
    const auto grandparent = makeEntity(reg, owner, {1, 0, 0});
    const auto parent = makeEntity(reg, owner, {2, 0, 0});
    const auto child = makeEntity(reg, owner, {3, 0, 0});
    const auto outsider = makeEntity(reg, owner, {0, 0, 0});
    if (!Hierarchy::attach(reg, parent, grandparent) || !Hierarchy::attach(reg, child, parent))
        return fail(name, "setup failed");
    fence.entities = {parent};
    reg.ctx().get<RetirementFenceBinding>().fence = &fence;
    if (Hierarchy::attach(reg, outsider, parent) || Hierarchy::attach(reg, parent, outsider))
        return fail(name, "attach accepted a retiring endpoint");
    const auto before = Hierarchy::computeWorldMatrix(reg, child);
    if (!Hierarchy::retireEntity(reg, parent, fence) || Hierarchy::parentOf(reg, child) != grandparent ||
        !nearMat(before, Hierarchy::computeWorldMatrix(reg, child)))
        return fail(name, "privileged orphan policy failed to preserve the surviving child");
    return pass(name);
}
} // namespace

int main()
{
    int failures = 0;
    if (!testRetirementHierarchyFence()) ++failures;
    if (!testRenderWorldPoseResolver()) ++failures;
    if (!testRenderWorldPoseInvalidParents()) ++failures;
    if (!testRenderWorldPoseCycle()) ++failures;
    if (!testIdentityPreconditions()) ++failures;
    if (!testRejectedReattachPreservesOriginalLink()) ++failures;
    if (!testAttachPropagate()) ++failures;
    if (!testRotation()) ++failures;
    if (!testKeepWorldAttach()) ++failures;
    if (!testCycleRejected()) ++failures;
    if (!testDetach()) ++failures;
    if (!testCascadeDestroy()) ++failures;
    if (!testOrphanDestroy()) ++failures;
    if (!testReparentToRoot()) ++failures;
    if (!testStaleParentAndRebuild()) ++failures;
    if (!testCohortRebuildPreservesUnrelatedMirrors()) ++failures;
    if (!testDepthLimit()) ++failures;
    if (!testPhysicsAttachmentPolicyRejectsBeforeMutation()) ++failures;
    if (failures == 0)
        std::cout << "All HierarchyTests passed" << std::endl;
    else
        std::cerr << failures << " HierarchyTests failed" << std::endl;
    return failures == 0 ? 0 : 1;
}
