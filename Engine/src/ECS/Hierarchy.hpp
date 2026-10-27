#pragma once

// Hierarchy API + transform propagation (hierarchy.md sections 3-5, D8).
// Header-only, EngineCore, generic and FPS-free. TransformComponent is the
// local transform; WorldTransformComponent is the derived world transform.
// ParentComponent/ChildrenComponent mirrors are maintained ONLY here (and
// by EntityLifecycle's destroy handling, which goes through this API).

#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace Hierarchy
{
inline constexpr int kMaxDepth = 64;

// Physics keeps world-space Jolt state outside ECS, while hierarchy transforms
// are local ECS data. These descriptors let direct hierarchy edits and staged
// level loading enforce one policy without a PhysicsSystem dependency.
struct PhysicsAttachmentNode
{
    bool has_rigid_body = false;
    BodyMotionType rigid_body_motion = BodyMotionType::Dynamic;
    bool has_character_controller = false;
    // Static collider, trigger, or terrain collision bodies do not have a
    // kinematic RigidBodyComponent authoring path.
    bool has_non_kinematic_physics_body = false;
    glm::vec3 local_scale = glm::vec3(1.0f);
};

enum class PhysicsAttachmentValidationResult
{
    Allowed,
    ChildHasCharacterController,
    ChildHasNonKinematicBody,
    AncestorHasNonUnitScale,
    InvalidAncestorChain,
};

inline bool hasUnitPhysicsAncestorScale(const glm::vec3& scale)
{
    constexpr float epsilon = 1e-4f;
    return std::abs(scale.x - 1.0f) <= epsilon &&
           std::abs(scale.y - 1.0f) <= epsilon &&
           std::abs(scale.z - 1.0f) <= epsilon;
}

// Shared data-only predicate for every hierarchy attachment path. A child
// without a physics body remains eligible; a child physics body must be a
// kinematic RigidBodyComponent. CharacterVirtual, dynamic/static, trigger,
// and terrain bodies stay roots. Each parent ancestor must have unit local
// scale so the child body has a stable world-pose contract.
inline PhysicsAttachmentValidationResult validatePhysicsAttachment(
    const PhysicsAttachmentNode& child,
    const std::vector<PhysicsAttachmentNode>& ancestors)
{
    if (child.has_character_controller)
        return PhysicsAttachmentValidationResult::ChildHasCharacterController;
    if (child.has_non_kinematic_physics_body ||
        (child.has_rigid_body &&
         child.rigid_body_motion != BodyMotionType::Kinematic))
    {
        return PhysicsAttachmentValidationResult::ChildHasNonKinematicBody;
    }
    // Ordinary hierarchy children have no Jolt body whose scale contract can
    // be violated, so ancestor scale is irrelevant to them. Only an accepted
    // kinematic RigidBodyComponent needs the unit-scale requirement.
    if (!child.has_rigid_body)
        return PhysicsAttachmentValidationResult::Allowed;

    for (const PhysicsAttachmentNode& ancestor : ancestors)
    {
        if (!hasUnitPhysicsAncestorScale(ancestor.local_scale))
            return PhysicsAttachmentValidationResult::AncestorHasNonUnitScale;
    }
    return PhysicsAttachmentValidationResult::Allowed;
}

namespace detail
{
inline EntityRef refOfEntity(entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e))
        return EntityRef{};
    const auto* id = r.try_get<IdentityComponent>(e);
    return id ? EntityRef{id->guid} : EntityRef{};
}

inline bool getUniqueIdentityGuid(entt::registry& r, entt::entity e,
                                  const char* role, EntityGuid& guid)
{
    const auto* identity = r.try_get<IdentityComponent>(e);
    if (!identity)
    {
        std::fprintf(stderr,
                     "[Hierarchy] attach rejected: %s entity has no IdentityComponent\n",
                     role);
        return false;
    }
    if (identity->guid.isNull())
    {
        std::fprintf(stderr,
                     "[Hierarchy] attach rejected: %s entity has a null IdentityComponent GUID\n",
                     role);
        return false;
    }

    std::size_t matches = 0;
    for (auto [candidate, candidate_identity] :
         r.view<IdentityComponent>().each())
    {
        (void)candidate;
        if (candidate_identity.guid == identity->guid)
            ++matches;
    }
    if (matches != 1)
    {
        std::fprintf(stderr,
                     "[Hierarchy] attach rejected: %s IdentityComponent GUID is duplicated in the registry\n",
                     role);
        return false;
    }

    guid = identity->guid;
    return true;
}

inline entt::entity resolveRef(entt::registry& r, EntityRef ref)
{
    if (ref.isNull())
        return entt::null;
    // O(n) scan: hierarchies are small and resolution happens at
    // attach/propagation boundaries, not per-component per frame.
    // (EntityLifecycle::resolve offers the O(1) indexed path.)
    for (auto [e, id] : r.view<IdentityComponent>().each())
    {
        if (id.guid == ref.guid)
            return r.valid(e) ? e : entt::null;
    }
    return entt::null;
}

inline glm::mat4 localMatrix(entt::registry& r, entt::entity e)
{
    const auto* t = r.try_get<TransformComponent>(e);
    return t ? t->getTransformMatrix() : glm::mat4(1.0f);
}

inline bool decomposeToLocal(const glm::mat4& m, TransformComponent& out)
{
    glm::vec3 scale{}, translation{}, skew{};
    glm::quat rotation{};
    glm::vec4 perspective{};
    if (!glm::decompose(m, scale, rotation, translation, skew, perspective))
        return false;
    if (std::abs(scale.x) < 1e-8f || std::abs(scale.y) < 1e-8f ||
        std::abs(scale.z) < 1e-8f)
        return false;
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(rotation), yaw, pitch, roll);
    out.position = translation;
    out.rotation = glm::vec3(glm::degrees(pitch), glm::degrees(yaw),
                             glm::degrees(roll));
    out.scale = scale;
    return true;
}

inline void eraseChildRef(ChildrenComponent& children, EntityRef child_ref)
{
    auto& v = children.children;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const EntityRef& c) { return c == child_ref; }),
            v.end());
}

inline bool containsChild(const ChildrenComponent& children, EntityRef child_ref)
{
    for (const EntityRef& c : children.children)
        if (c == child_ref)
            return true;
    return false;
}

inline PhysicsAttachmentNode physicsAttachmentNodeFor(const entt::registry& r,
                                                       entt::entity entity)
{
    PhysicsAttachmentNode node;
    if (const auto* transform = r.try_get<TransformComponent>(entity))
        node.local_scale = transform->scale;
    if (const auto* rigid_body = r.try_get<RigidBodyComponent>(entity))
    {
        node.has_rigid_body = true;
        node.rigid_body_motion = rigid_body->motion_type;
    }
    node.has_character_controller =
        r.all_of<CharacterControllerComponent>(entity);

    const auto* collider = r.try_get<ColliderComponent>(entity);
    const bool has_static_collider =
        !node.has_rigid_body && collider != nullptr;
    // LevelManager currently creates a static mesh body for this combination,
    // even when the authoring RigidBody motion is Kinematic.
    const bool has_kinematic_mesh_body =
        node.has_rigid_body &&
        node.rigid_body_motion == BodyMotionType::Kinematic && collider &&
        collider->shape_type == ColliderShapeType::Mesh;
    const auto* terrain = r.try_get<TerrainComponent>(entity);
    node.has_non_kinematic_physics_body =
        has_static_collider || has_kinematic_mesh_body ||
        (!node.has_rigid_body && r.all_of<TriggerComponent>(entity)) ||
        (terrain && terrain->generate_collision);
    return node;
}
} // namespace detail

inline entt::entity parentOf(entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e))
        return entt::null;
    const auto* p = r.try_get<ParentComponent>(e);
    if (!p || p->parent.isNull())
        return entt::null;
    return detail::resolveRef(r, p->parent);
}

// Runtime counterpart to the staged-data predicate above. Missing parent refs
// end the chain like normal hierarchy resolution; cycles or a depth overflow
// cannot be safely inspected and reject the edit.
inline PhysicsAttachmentValidationResult validatePhysicsAttachment(
    entt::registry& r, entt::entity child, entt::entity parent)
{
    if (child == entt::null || parent == entt::null || !r.valid(child) ||
        !r.valid(parent))
    {
        return PhysicsAttachmentValidationResult::InvalidAncestorChain;
    }

    std::vector<PhysicsAttachmentNode> ancestors;
    ancestors.reserve(kMaxDepth);
    std::unordered_set<entt::entity> visited;
    entt::entity current = parent;
    while (current != entt::null && r.valid(current) &&
           ancestors.size() < static_cast<std::size_t>(kMaxDepth))
    {
        if (!visited.insert(current).second)
            return PhysicsAttachmentValidationResult::InvalidAncestorChain;
        ancestors.push_back(detail::physicsAttachmentNodeFor(r, current));
        current = parentOf(r, current);
    }
    if (current != entt::null && r.valid(current))
        return PhysicsAttachmentValidationResult::InvalidAncestorChain;

    return validatePhysicsAttachment(detail::physicsAttachmentNodeFor(r, child),
                                     ancestors);
}

// Cycle-safe ancestor walk, max depth 64 (hierarchy.md section 3).
inline bool isDescendantOf(entt::registry& r, entt::entity e,
                           entt::entity ancestor)
{
    if (e == entt::null || ancestor == entt::null)
        return false;
    std::unordered_set<entt::entity> visited;
    entt::entity cur = e;
    for (int depth = 0; depth < kMaxDepth; ++depth)
    {
        if (!visited.insert(cur).second)
            return false; // existing cycle: stop, do not claim descent
        entt::entity p = parentOf(r, cur);
        if (p == entt::null)
            return false;
        if (p == ancestor)
            return true;
        cur = p;
    }
    return false;
}

// World matrix composed from local transforms up the ancestor chain.
// Independent of the propagation cache, so keep_world works mid-frame.
inline glm::mat4 computeWorldMatrix(entt::registry& r, entt::entity e)
{
    std::vector<entt::entity> chain;
    std::unordered_set<entt::entity> visited;
    entt::entity cur = e;
    while (cur != entt::null && r.valid(cur) &&
           static_cast<int>(chain.size()) < kMaxDepth &&
           visited.insert(cur).second)
    {
        chain.push_back(cur);
        cur = parentOf(r, cur);
    }
    glm::mat4 world(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        world = world * detail::localMatrix(r, *it);
    return world;
}

// Simulation-space position composed from TransformComponent local poses.
// PresentationTransformComponent is intentionally not consulted here: render
// interpolation must not move gameplay-facing consumers such as audio.
inline std::optional<glm::vec3> resolveSimulationWorldPosition(
    entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e) ||
        !r.all_of<TransformComponent>(e))
    {
        return std::nullopt;
    }
    return glm::vec3(computeWorldMatrix(r, e)[3]);
}

namespace detail
{
// Resolve only unambiguous stable references. A duplicated identity makes a
// runtime link ambiguous, so render resolution treats it like an unresolved
// parent and roots the child at its local pose.
inline entt::entity resolveUniqueRenderRef(const entt::registry& r,
                                           EntityRef ref)
{
    if (ref.isNull())
        return entt::null;

    entt::entity match = entt::null;
    for (auto [candidate, identity] : r.view<IdentityComponent>().each())
    {
        if (identity.guid != ref.guid)
            continue;
        if (match != entt::null)
            return entt::null;
        match = candidate;
    }
    return match != entt::null && r.valid(match) ? match : entt::null;
}

inline glm::mat4 renderLocalMatrix(const entt::registry& r, entt::entity e)
{
    return resolveRenderTransform(r, e).getTransformMatrix();
}
} // namespace detail

// Ephemeral render pose resolved from current ECS state. Presentation values
// overlay local position/rotation while scale remains authoritative on
// TransformComponent. Full matrices are composed so inherited non-uniform
// scale and shear survive; the derived WorldTransformComponent cache is never
// read or written.
//
// Missing, null, stale, or ambiguous parent refs terminate the walk and make
// that entity the root of the resolved chain. Invalid requested entities,
// cycles, and chains deeper than kMaxDepth return no pose; choosing a cycle
// break would make the same invalid hierarchy resolve differently depending
// on which entity was queried. No fallback mutates ECS data or retains a
// prior pose.
inline std::optional<glm::mat4> resolveRenderWorldMatrix(
    const entt::registry& r, entt::entity entity)
{
    if (entity == entt::null || !r.valid(entity))
        return std::nullopt;

    std::vector<entt::entity> chain;
    chain.reserve(kMaxDepth);
    std::unordered_set<entt::entity> visited;
    entt::entity current = entity;
    bool cycle_detected = false;

    while (current != entt::null && r.valid(current) &&
           chain.size() < static_cast<std::size_t>(kMaxDepth))
    {
        if (!visited.insert(current).second)
        {
            cycle_detected = true;
            break;
        }
        chain.push_back(current);

        const auto* parent = r.try_get<ParentComponent>(current);
        if (!parent || parent->parent.isNull())
        {
            current = entt::null;
            break;
        }
        current = detail::resolveUniqueRenderRef(r, parent->parent);
        if (current == entt::null)
            break;
    }

    if (cycle_detected ||
        (chain.size() == static_cast<std::size_t>(kMaxDepth) &&
         current != entt::null && r.valid(current)))
        return std::nullopt;

    glm::mat4 world(1.0f);
    for (std::size_t i = chain.size(); i > 0; --i)
        world *= detail::renderLocalMatrix(r, chain[i - 1]);
    return world;
}

inline void markDirty(entt::registry& r, entt::entity e);

namespace detail
{
inline void markDirtyRecursive(entt::registry& r, entt::entity e, int depth,
                               std::unordered_set<entt::entity>& visited)
{
    if (e == entt::null || !r.valid(e) || depth > kMaxDepth)
        return;
    if (!visited.insert(e).second)
        return;
    r.emplace_or_replace<HierarchyDirtyTag>(e);
    if (auto* wt = r.try_get<WorldTransformComponent>(e))
        wt->dirty = true;
    const auto* children = r.try_get<ChildrenComponent>(e);
    if (!children)
        return;
    const std::vector<EntityRef> snapshot = children->children;
    for (EntityRef ref : snapshot)
    {
        entt::entity child = resolveRef(r, ref);
        if (child != entt::null)
            markDirtyRecursive(r, child, depth + 1, visited);
    }
}
} // namespace detail

// Marks e + whole subtree dirty (hierarchy.md section 3).
inline void markDirty(entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e))
        return;
    std::unordered_set<entt::entity> visited;
    detail::markDirtyRecursive(r, e, 0, visited);
}

// Removes e from its parent's Children list only; ParentComponent and
// transforms are untouched. Used by EntityLifecycle before destroying e.
inline void unlinkFromParentList(entt::registry& r, entt::entity e)
{
    entt::entity parent = parentOf(r, e);
    if (parent == entt::null)
        return;
    if (auto* children = r.try_get<ChildrenComponent>(parent))
        detail::eraseChildRef(*children, detail::refOfEntity(r, e));
}

// Returns true only when linked. Both entities must have distinct, non-null
// GUIDs that occur exactly once in the registry.
inline bool attach(entt::registry& r, entt::entity child,
                   entt::entity parent, bool keep_world = false)
{
    if (child == entt::null || parent == entt::null || !r.valid(child) ||
        !r.valid(parent))
    {
        std::fprintf(stderr, "[Hierarchy] attach rejected: invalid entity\n");
        return false;
    }
    EntityGuid child_guid{};
    EntityGuid parent_guid{};
    if (!detail::getUniqueIdentityGuid(r, child, "child", child_guid) ||
        !detail::getUniqueIdentityGuid(r, parent, "parent", parent_guid))
    {
        return false;
    }
    if (child == parent || isDescendantOf(r, parent, child))
    {
        std::fprintf(stderr, "[Hierarchy] attach rejected: cycle\n");
        return false;
    }
    const PhysicsAttachmentValidationResult physics_validation =
        validatePhysicsAttachment(r, child, parent);
    if (physics_validation != PhysicsAttachmentValidationResult::Allowed)
    {
        std::fprintf(stderr,
                     "[Hierarchy] attach rejected: physics hierarchy policy (%d)\n",
                     static_cast<int>(physics_validation));
        return false;
    }
    // keep_world is computed against the OLD ancestry before relinking.
    glm::mat4 child_world(1.0f);
    if (keep_world)
        child_world = computeWorldMatrix(r, child);

    if (r.all_of<ParentComponent>(child))
        unlinkFromParentList(r, child);

    if (keep_world)
    {
        const glm::mat4 parent_world = computeWorldMatrix(r, parent);
        if (std::abs(glm::determinant(parent_world)) > 1e-8f)
        {
            auto& local = r.get_or_emplace<TransformComponent>(child);
            detail::decomposeToLocal(glm::inverse(parent_world) * child_world,
                                     local);
        }
    }

    r.emplace_or_replace<ParentComponent>(child,
                                          ParentComponent{EntityRef{parent_guid}});
    auto& children = r.get_or_emplace<ChildrenComponent>(parent);
    const EntityRef child_ref{child_guid};
    if (!child_ref.isNull() && !detail::containsChild(children, child_ref))
        children.children.push_back(child_ref);
    if (!r.all_of<WorldTransformComponent>(child))
        r.emplace<WorldTransformComponent>(child);
    if (!r.all_of<WorldTransformComponent>(parent))
        r.emplace<WorldTransformComponent>(parent);
    markDirty(r, child);
    markDirty(r, parent);
    return true;
}

inline void detach(entt::registry& r, entt::entity child,
                   bool keep_world = true)
{
    if (child == entt::null || !r.valid(child))
        return;
    if (!r.all_of<ParentComponent>(child))
        return;
    glm::mat4 world(1.0f);
    if (keep_world)
        world = computeWorldMatrix(r, child);
    unlinkFromParentList(r, child);
    r.remove<ParentComponent>(child);
    if (keep_world)
    {
        auto& local = r.get_or_emplace<TransformComponent>(child);
        detail::decomposeToLocal(world, local);
    }
    if (!r.all_of<WorldTransformComponent>(child))
        r.emplace<WorldTransformComponent>(child);
    markDirty(r, child);
}

// ---------------------------------------------------------------------------
// Transform propagation (hierarchy.md section 4, Phase 1 rule).
// Every entity with a ParentComponent or ChildrenComponent is propagated
// every tick (hierarchies are small); roots without hierarchy are skipped.
// Roots: local == world. Deterministic root order sorted by EntityRef GUID.
// Depth-first; chains deeper than 64 stop propagating below the limit and
// log once per root; that subtree stays frozen (documented failure case).
// ---------------------------------------------------------------------------
namespace detail
{
inline void propagateRecursive(entt::registry& r, entt::entity e,
                               const glm::mat4& parent_matrix, int depth,
                               bool& depth_logged,
                               std::unordered_set<entt::entity>& visited)
{
    if (e == entt::null || !r.valid(e))
        return;
    if (depth > kMaxDepth)
    {
        if (!depth_logged)
        {
            std::fprintf(stderr,
                         "[Hierarchy] propagation stopped: depth > 64\n");
            depth_logged = true;
        }
        return;
    }
    if (!visited.insert(e).second)
        return;
    const glm::mat4 world = parent_matrix * localMatrix(r, e);
    auto& wt = r.get_or_emplace<WorldTransformComponent>(e);
    wt.matrix = world;
    glm::vec3 scale{}, translation{}, skew{};
    glm::quat rotation{};
    glm::vec4 perspective{};
    if (glm::decompose(world, scale, rotation, translation, skew, perspective))
    {
        wt.position = translation;
        wt.rotation = rotation;
        wt.scale = scale;
    }
    else
    {
        wt.position = glm::vec3(world[3]);
        wt.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        wt.scale = glm::vec3(1.0f);
    }
    wt.dirty = false;
    if (r.all_of<HierarchyDirtyTag>(e))
        r.remove<HierarchyDirtyTag>(e);

    const auto* children_comp = r.try_get<ChildrenComponent>(e);
    if (!children_comp)
        return;
    const std::vector<EntityRef> snapshot = children_comp->children;
    std::vector<EntityRef> stale;
    for (EntityRef ref : snapshot)
    {
        entt::entity child = resolveRef(r, ref);
        if (child == entt::null ||
            child == entt::null)
        {
            stale.push_back(ref); // lazily pruned below (hierarchy.md §6)
            continue;
        }
        if (!r.valid(child))
        {
            stale.push_back(ref);
            continue;
        }
        propagateRecursive(r, child, world, depth + 1, depth_logged, visited);
    }
    if (!stale.empty())
    {
        if (auto* live = r.try_get<ChildrenComponent>(e))
            for (EntityRef ref : stale)
                eraseChildRef(*live, ref);
    }
}
} // namespace detail

inline void updateTransformPropagation(entt::registry& r)
{
    std::vector<entt::entity> roots;
    for (entt::entity e : r.view<TransformComponent>())
    {
        const bool hierarchied = r.all_of<ParentComponent>(e) ||
                                 r.all_of<ChildrenComponent>(e);
        if (!hierarchied)
            continue;
        if (parentOf(r, e) == entt::null)
            roots.push_back(e);
    }
    std::sort(roots.begin(), roots.end(),
              [&r](entt::entity a, entt::entity b) {
                  const EntityGuid ga = detail::refOfEntity(r, a).guid;
                  const EntityGuid gb = detail::refOfEntity(r, b).guid;
                  if (ga.hi != gb.hi) return ga.hi < gb.hi;
                  if (ga.lo != gb.lo) return ga.lo < gb.lo;
                  return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
              });
    std::unordered_set<entt::entity> visited;
    for (entt::entity root : roots)
    {
        bool depth_logged = false;
        detail::propagateRecursive(r, root, glm::mat4(1.0f), 0, depth_logged,
                                   visited);
    }
}

// ---------------------------------------------------------------------------
// All descendants of e (depth-first, cycle-safe, depth limit 64).
inline std::vector<entt::entity> collectDescendants(entt::registry& r,
                                                    entt::entity e)
{
    std::vector<entt::entity> out;
    std::unordered_set<entt::entity> visited;
    std::function<void(entt::entity, int)> walk =
        [&](entt::entity cur, int depth) {
            if (cur == entt::null || !r.valid(cur) || depth > kMaxDepth)
                return;
            if (!visited.insert(cur).second)
                return;
            const auto* children = r.try_get<ChildrenComponent>(cur);
            if (!children)
                return;
            const std::vector<EntityRef> snapshot = children->children;
            for (EntityRef ref : snapshot)
            {
                entt::entity child = detail::resolveRef(r, ref);
                if (child == entt::null || !r.valid(child))
                    continue;
                out.push_back(child);
                walk(child, depth + 1);
            }
        };
    walk(e, 0);
    return out;
}

// Serialization rebuild (hierarchy.md section 5). The ordered list is one
// load cohort: only its ChildrenComponent mirrors are cleared, and parent
// references resolve only to identities in that cohort. ParentComponent is
// authoritative; ChildrenComponent is rebuilt in the supplied file order.
// WorldTransformComponent is derived and is never saved.
// ---------------------------------------------------------------------------
inline void rebuildChildren(entt::registry& r,
                            const std::vector<entt::entity>& ordered_children)
{
    std::unordered_map<EntityGuid, entt::entity, GuidHash> cohort_by_guid;
    cohort_by_guid.reserve(ordered_children.size());
    for (entt::entity e : ordered_children)
    {
        if (e == entt::null || !r.valid(e))
            continue;
        const auto* identity = r.try_get<IdentityComponent>(e);
        if (identity && !identity->guid.isNull())
            cohort_by_guid.emplace(identity->guid, e);
    }

    // Serialized ChildrenComponent data is only a mirror. Clear mirrors on
    // cohort entities and leave every unrelated registry entity untouched.
    for (entt::entity e : ordered_children)
    {
        if (e == entt::null || !r.valid(e))
            continue;
        if (auto* c = r.try_get<ChildrenComponent>(e))
            c->children.clear();
    }

    for (entt::entity child : ordered_children)
    {
        if (child == entt::null || !r.valid(child))
            continue;
        const auto* p = r.try_get<ParentComponent>(child);
        if (!p || p->parent.isNull())
            continue;
        auto parent_it = cohort_by_guid.find(p->parent.guid);
        if (parent_it == cohort_by_guid.end())
        {
            std::fprintf(stderr,
                         "[Hierarchy] rebuildChildren: parent is outside the load cohort, "
                         "entity becomes root\n");
            r.remove<ParentComponent>(child);
            continue;
        }
        const entt::entity parent = parent_it->second;
        auto& children = r.get_or_emplace<ChildrenComponent>(parent);
        const EntityRef child_ref = detail::refOfEntity(r, child);
        if (!child_ref.isNull() &&
            !detail::containsChild(children, child_ref))
            children.children.push_back(child_ref);
        if (!r.all_of<WorldTransformComponent>(child))
            r.emplace<WorldTransformComponent>(child);
        if (!r.all_of<WorldTransformComponent>(parent))
            r.emplace<WorldTransformComponent>(parent);
    }
    // Only this load cohort is dirtied. Its rebuilt mirrors contain cohort
    // children, so this cannot walk into unrelated registry hierarchies.
    for (entt::entity e : ordered_children)
        if (e != entt::null && r.valid(e) &&
            r.any_of<ParentComponent, ChildrenComponent>(e))
            markDirty(r, e);
}

// Rebuilds the whole registry in registry order for callers without a saved
// cohort. File loaders should use the ordered overload so mirrors preserve
// LevelData file order and unrelated registry entities remain untouched.
inline void rebuildChildren(entt::registry& r)
{
    std::vector<entt::entity> ordered;
    for (entt::entity e : r.view<entt::entity>())
        ordered.push_back(e);
    rebuildChildren(r, ordered);
}

} // namespace Hierarchy
