#pragma once

#include "ECS/Hierarchy.hpp"
#include "Frustum.hpp"
#include "Components/Components.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_set>

// Forward declarations
class mesh;
struct TransformComponent;
struct MeshComponent;

// BVH Node structure
struct BVHNode
{
    AABB bounds;
    int left_child = -1;   // Index of left child, or -1 if leaf
    int right_child = -1;  // Index of right child, or -1 if leaf
    entt::entity entity = entt::null;  // Entity reference (only valid for leaf nodes)

    bool isLeaf() const { return entity != entt::null; }
};

struct CullStats
{
    uint64_t builds = 0;
    uint64_t entities_scanned = 0;
    mutable uint64_t nodes_visited = 0;
    mutable uint64_t aabb_tests = 0;
    mutable uint64_t leaves_collected = 0;
};

// Scene BVH for frustum culling
class SceneBVH
{
public:
    SceneBVH() = default;

    // Build cached BVH bounds for static root renderables. Parent/presentation
    // entities use live poses through the registry-aware query overload.
    void build(entt::registry& registry);

    // Query entities represented by the cached static BVH bounds.
    void queryFrustum(const Frustum& frustum, std::vector<entt::entity>& results) const;

    // Presentation-aware render query. Static authoritative bounds stay in the
    // BVH; presentation entities are checked at their current pose separately.
    void queryFrustum(const Frustum& frustum, const entt::registry& registry,
                      std::vector<entt::entity>& results) const;

    // Pick the closest entity hit by a ray. Returns entt::null if nothing hit.
    entt::entity rayPick(const glm::vec3& origin, const glm::vec3& direction) const;

    // Registry-aware picking keeps cached root bounds while resolving parented
    // and presentation-driven renderables from their current ECS pose.
    entt::entity rayPick(const glm::vec3& origin, const glm::vec3& direction,
                         const entt::registry& registry) const;

    // Mark the BVH as needing rebuild (e.g., when entities move or are added/removed)
    void markDirty() { dirty = true; }

    // Check if BVH needs to be rebuilt
    bool needsRebuild() const { return dirty; }

    // Get statistics
    size_t getNodeCount() const { return nodes.size(); }
    size_t getTotalEntities() const { return entity_count; }
    const CullStats& getCullStats() const { return cull_stats; }
    void resetCullStats() { cull_stats = CullStats{}; }

private:
    CullStats cull_stats{};
    std::vector<BVHNode> nodes;
    int root_index = -1;
    bool dirty = true;
    size_t entity_count = 0;
    // These entities are resolved against current ECS links at query time.
    // Keeping only their handles lets the registry-aware path include an
    // entity that was detached after build without retaining an old pose.
    std::unordered_set<entt::entity> live_pose_entities;

    // Internal build data structure
    struct EntityBounds
    {
        entt::entity entity;
        AABB world_bounds;
        glm::vec3 centroid;
    };

    // Recursive build function (top-down median split)
    int buildRecursive(std::vector<EntityBounds>& entities, size_t start, size_t end);

    // Recursive frustum query
    void queryFrustumRecursive(int nodeIndex, const Frustum& frustum,
                               const entt::registry* registry,
                               std::vector<entt::entity>& results) const;

    // Collect all leaf entities in a subtree (no frustum tests)
    void collectAllLeaves(int nodeIndex, const entt::registry* registry,
                          std::vector<entt::entity>& results) const;

    // Recursive ray pick (returns closest hit entity)
    void rayPickRecursive(int nodeIndex, const glm::vec3& origin, const glm::vec3& direction,
                          const entt::registry* registry, float& closest_t,
                          entt::entity& closest_entity) const;

    bool usesLivePose(const entt::registry& registry, entt::entity entity) const;
};

// Implementation

namespace SceneBVHDetail
{
inline bool isFinite(const glm::mat4& matrix)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(matrix[column][row]))
                return false;
    return true;
}

inline bool isFinite(const AABB& bounds)
{
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]))
            return false;
    return bounds.isValid();
}

inline bool isFinite(const glm::vec3& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}
} // namespace SceneBVHDetail

inline bool SceneBVH::usesLivePose(const entt::registry& registry,
                                   entt::entity entity) const
{
    return !registry.valid(entity) ||
           live_pose_entities.find(entity) != live_pose_entities.end() ||
           registry.any_of<ParentComponent, PresentationTransformComponent>(entity);
}

inline void SceneBVH::build(entt::registry& registry)
{
    cull_stats.builds++;
    nodes.clear();
    root_index = -1;
    entity_count = 0;
    dirty = false;
    live_pose_entities.clear();

    // Collect all entities with MeshComponent and TransformComponent
    std::vector<EntityBounds> entity_bounds;
    auto view = registry.view<MeshComponent, TransformComponent>();

    for (auto entity : view)
    {
        auto& mesh_comp = view.get<MeshComponent>(entity);
        if (registry.any_of<ParentComponent, PresentationTransformComponent>(entity))
            live_pose_entities.insert(entity);

        // Skip entities without valid meshes
        if (!mesh_comp.m_mesh || !mesh_comp.m_mesh->visible || !mesh_comp.m_mesh->is_valid)
        {
            continue;
        }

        mesh* m = mesh_comp.m_mesh.get();

        // Compute local AABB if not already computed
        if (!m->bounds_computed)
        {
            m->computeBounds();
        }

        const auto render_world = Hierarchy::resolveRenderWorldMatrix(registry, entity);
        if (!render_world || !SceneBVHDetail::isFinite(*render_world))
            continue;

        // Parent/presentation poses are evaluated against current ECS state
        // during registry-aware culling. Do not retain their bounds here.
        if (live_pose_entities.find(entity) != live_pose_entities.end())
            continue;

        // Transform local AABB to world space using the shared render pose.
        AABB world_aabb = AABB::fromTransformedAABB(
            m->aabb_min, m->aabb_max, *render_world);
        if (!SceneBVHDetail::isFinite(world_aabb))
            continue;

        EntityBounds eb;
        eb.entity = entity;
        eb.world_bounds = world_aabb;
        eb.centroid = world_aabb.getCenter();
        entity_bounds.push_back(eb);
    }

    entity_count = entity_bounds.size();
    cull_stats.entities_scanned += (uint64_t)entity_count;

    if (entity_bounds.empty())
    {
        return;
    }

    // Reserve space for nodes (worst case: 2n-1 nodes for n entities)
    nodes.reserve(entity_bounds.size() * 2);

    // Build the BVH recursively
    root_index = buildRecursive(entity_bounds, 0, entity_bounds.size());
}

inline int SceneBVH::buildRecursive(std::vector<EntityBounds>& entities, size_t start, size_t end)
{
    if (start >= end)
    {
        return -1;
    }

    // Create a new node
    int node_index = static_cast<int>(nodes.size());
    nodes.emplace_back();
    BVHNode& node = nodes[node_index];

    // Compute bounds for all entities in this range
    for (size_t i = start; i < end; ++i)
    {
        node.bounds.expand(entities[i].world_bounds);
    }

    size_t count = end - start;

    // If only 1-2 entities, create leaf nodes
    if (count <= 2)
    {
        // Create a leaf for the first entity
        node.entity = entities[start].entity;

        // If there's a second entity, create a sibling leaf
        if (count == 2)
        {
            // Change this node to an internal node
            node.entity = entt::null;

            // Create left leaf
            int left_index = static_cast<int>(nodes.size());
            nodes.emplace_back();
            nodes[left_index].bounds = entities[start].world_bounds;
            nodes[left_index].entity = entities[start].entity;

            // Create right leaf
            int right_index = static_cast<int>(nodes.size());
            nodes.emplace_back();
            nodes[right_index].bounds = entities[start + 1].world_bounds;
            nodes[right_index].entity = entities[start + 1].entity;

            // Update current node (need to re-reference as vector may have grown)
            nodes[node_index].left_child = left_index;
            nodes[node_index].right_child = right_index;
        }

        return node_index;
    }

    // Find the longest axis of the combined bounds
    glm::vec3 extent = node.bounds.getSize();
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > extent[axis]) axis = 2;

    // Sort entities by centroid along the chosen axis
    std::sort(entities.begin() + start, entities.begin() + end,
        [axis](const EntityBounds& a, const EntityBounds& b) {
            return a.centroid[axis] < b.centroid[axis];
        });

    // Split at median
    size_t mid = start + count / 2;

    // Recursively build children
    int left_index = buildRecursive(entities, start, mid);
    int right_index = buildRecursive(entities, mid, end);

    // Update node with child indices (re-reference as vector may have grown)
    nodes[node_index].left_child = left_index;
    nodes[node_index].right_child = right_index;

    return node_index;
}

inline void SceneBVH::queryFrustum(const Frustum& frustum, std::vector<entt::entity>& results) const
{
    results.clear();

    if (root_index < 0 || nodes.empty())
    {
        return;
    }

    queryFrustumRecursive(root_index, frustum, nullptr, results);
}

inline void SceneBVH::queryFrustum(const Frustum& frustum, const entt::registry& registry,
                                   std::vector<entt::entity>& results) const
{
    results.clear();

    if (root_index >= 0 && !nodes.empty())
        queryFrustumRecursive(root_index, frustum, &registry, results);

    // Parent links and presentation overrides can change independently of the
    // static BVH bounds. Resolve those entities from current ECS data each
    // query, including entities that were parented/presented at build time
    // and have since become roots.
    auto render_view = registry.view<MeshComponent, TransformComponent>();
    for (entt::entity entity : render_view)
    {
        if (!usesLivePose(registry, entity))
            continue;

        const auto& mesh_comp = render_view.get<MeshComponent>(entity);
        if (!mesh_comp.m_mesh || !mesh_comp.m_mesh->visible || !mesh_comp.m_mesh->is_valid)
            continue;

        mesh* m = mesh_comp.m_mesh.get();
        if (!m->bounds_computed)
            m->computeBounds();

        const auto render_world = Hierarchy::resolveRenderWorldMatrix(registry, entity);
        if (!render_world || !SceneBVHDetail::isFinite(*render_world))
            continue;

        const AABB bounds = AABB::fromTransformedAABB(m->aabb_min, m->aabb_max,
                                                       *render_world);
        if (SceneBVHDetail::isFinite(bounds) && frustum.intersectsAABB(bounds)) {
            results.push_back(entity);
            cull_stats.leaves_collected++;
        }
    }
}

inline void SceneBVH::collectAllLeaves(int nodeIndex, const entt::registry* registry,
                                       std::vector<entt::entity>& results) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) return;
    const BVHNode& node = nodes[nodeIndex];
    if (node.isLeaf()) {
        if (!registry || !usesLivePose(*registry, node.entity)) {
            results.push_back(node.entity);
            cull_stats.leaves_collected++;
        }
        return;
    }
    if (node.left_child >= 0) collectAllLeaves(node.left_child, registry, results);
    if (node.right_child >= 0) collectAllLeaves(node.right_child, registry, results);
}

inline void SceneBVH::queryFrustumRecursive(int nodeIndex, const Frustum& frustum,
                                            const entt::registry* registry,
                                            std::vector<entt::entity>& results) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size()))
    {
        return;
    }

    const BVHNode& node = nodes[nodeIndex];
    cull_stats.nodes_visited++;
    cull_stats.aabb_tests++;

    // Test this node's bounds against the frustum
    if (!frustum.intersectsAABB(node.bounds))
    {
        return;  // Entire subtree is outside frustum
    }

    // If fully inside frustum, add all leaves without further tests
    if (frustum.containsAABB(node.bounds))
    {
        collectAllLeaves(nodeIndex, registry, results);
        return;
    }

    // If this is a leaf node, add the entity to results
    if (node.isLeaf())
    {
        if (!registry || !usesLivePose(*registry, node.entity))
            results.push_back(node.entity);
        return;
    }

    // Recurse on children
    if (node.left_child >= 0)
    {
        queryFrustumRecursive(node.left_child, frustum, registry, results);
    }
    if (node.right_child >= 0)
    {
        queryFrustumRecursive(node.right_child, frustum, registry, results);
    }
}

inline entt::entity SceneBVH::rayPick(const glm::vec3& origin, const glm::vec3& direction) const
{
    const float direction_length_squared = glm::dot(direction, direction);
    if (!SceneBVHDetail::isFinite(origin) ||
        !SceneBVHDetail::isFinite(direction) ||
        !std::isfinite(direction_length_squared) ||
        direction_length_squared <= 1e-16f || root_index < 0 || nodes.empty())
        return entt::null;

    float closest_t = std::numeric_limits<float>::max();
    entt::entity closest_entity = entt::null;
    rayPickRecursive(root_index, origin, direction, nullptr, closest_t, closest_entity);
    return closest_entity;
}

inline entt::entity SceneBVH::rayPick(const glm::vec3& origin,
                                     const glm::vec3& direction,
                                     const entt::registry& registry) const
{
    const float direction_length_squared = glm::dot(direction, direction);
    if (!SceneBVHDetail::isFinite(origin) ||
        !SceneBVHDetail::isFinite(direction) ||
        !std::isfinite(direction_length_squared) ||
        direction_length_squared <= 1e-16f)
        return entt::null;

    float closest_t = std::numeric_limits<float>::max();
    entt::entity closest_entity = entt::null;
    if (root_index >= 0 && !nodes.empty())
        rayPickRecursive(root_index, origin, direction, &registry, closest_t,
                         closest_entity);

    // Cached leaves cannot represent a current hierarchy or presentation pose.
    // Resolve these renderables against live ECS values so parent movement and
    // presentation overlays are pickable without rebuilding the static BVH.
    auto render_view = registry.view<MeshComponent, TransformComponent>();
    for (entt::entity entity : render_view)
    {
        if (!usesLivePose(registry, entity))
            continue;

        const auto& mesh_comp = render_view.get<MeshComponent>(entity);
        if (!mesh_comp.m_mesh || !mesh_comp.m_mesh->visible ||
            !mesh_comp.m_mesh->is_valid)
            continue;

        mesh* m = mesh_comp.m_mesh.get();
        if (!m->bounds_computed)
            m->computeBounds();

        const auto render_world = Hierarchy::resolveRenderWorldMatrix(registry, entity);
        if (!render_world || !SceneBVHDetail::isFinite(*render_world))
            continue;

        const AABB bounds = AABB::fromTransformedAABB(m->aabb_min, m->aabb_max,
                                                       *render_world);
        if (!SceneBVHDetail::isFinite(bounds))
            continue;

        float t_min = 0.0f;
        if (!bounds.intersectsRay(origin, direction, t_min))
            continue;

        const float hit_t = std::max(t_min, 0.0f);
        if (hit_t < closest_t)
        {
            closest_t = hit_t;
            closest_entity = entity;
        }
    }

    return closest_entity;
}

inline void SceneBVH::rayPickRecursive(int nodeIndex, const glm::vec3& origin, const glm::vec3& direction,
                                        const entt::registry* registry,
                                        float& closest_t,
                                        entt::entity& closest_entity) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size()))
        return;

    const BVHNode& node = nodes[nodeIndex];

    // Test ray against this node's AABB
    float tMin;
    if (!node.bounds.intersectsRay(origin, direction, tMin))
        return; // Ray misses this subtree entirely

    // If the closest possible hit in this subtree is farther than our current best, skip
    if (tMin > closest_t)
        return;

    // Leaf node: this entity is a candidate
    if (node.isLeaf())
    {
        if (registry)
        {
            // Ignore deleted, hidden, or now-live-pose leaves: their cached
            // bounds are no longer authoritative for the current render pose.
            if (!registry->valid(node.entity) || usesLivePose(*registry, node.entity))
                return;
            const auto* mesh_comp = registry->try_get<MeshComponent>(node.entity);
            if (!mesh_comp || !mesh_comp->m_mesh || !mesh_comp->m_mesh->visible ||
                !mesh_comp->m_mesh->is_valid)
                return;
        }

        const float hit_t = std::max(tMin, 0.0f);
        if (hit_t < closest_t)
        {
            closest_t = hit_t;
            closest_entity = node.entity;
        }
        return;
    }

    // Recurse into both children
    if (node.left_child >= 0)
        rayPickRecursive(node.left_child, origin, direction, registry, closest_t,
                         closest_entity);
    if (node.right_child >= 0)
        rayPickRecursive(node.right_child, origin, direction, registry, closest_t,
                         closest_entity);
}
