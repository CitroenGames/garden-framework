#pragma once

#include "Frustum.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <algorithm>

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

// Scene BVH for frustum culling
class SceneBVH
{
public:
    SceneBVH() = default;

    // Build BVH from all entities with MeshComponent and TransformComponent
    void build(entt::registry& registry);

    // Query all entities visible in the frustum
    void queryFrustum(const Frustum& frustum, std::vector<entt::entity>& results) const;

    // Mark the BVH as needing rebuild (e.g., when entities move or are added/removed)
    void markDirty() { dirty = true; }

    // Check if BVH needs to be rebuilt
    bool needsRebuild() const { return dirty; }

    // Get statistics
    size_t getNodeCount() const { return nodes.size(); }
    size_t getTotalEntities() const { return entity_count; }

private:
    std::vector<BVHNode> nodes;
    int root_index = -1;
    bool dirty = true;
    size_t entity_count = 0;

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
    void queryFrustumRecursive(int nodeIndex, const Frustum& frustum, std::vector<entt::entity>& results) const;
};

// Implementation

inline void SceneBVH::build(entt::registry& registry)
{
    nodes.clear();
    root_index = -1;
    entity_count = 0;
    dirty = false;

    // Collect all entities with MeshComponent and TransformComponent
    std::vector<EntityBounds> entity_bounds;
    auto view = registry.view<MeshComponent, TransformComponent>();

    for (auto entity : view)
    {
        auto& mesh_comp = view.get<MeshComponent>(entity);
        const auto& transform = view.get<TransformComponent>(entity);

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

        // Transform local AABB to world space
        AABB world_aabb = AABB::fromTransformedAABB(m->aabb_min, m->aabb_max, transform.getTransformMatrix());

        EntityBounds eb;
        eb.entity = entity;
        eb.world_bounds = world_aabb;
        eb.centroid = world_aabb.getCenter();
        entity_bounds.push_back(eb);
    }

    entity_count = entity_bounds.size();

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

    queryFrustumRecursive(root_index, frustum, results);
}

inline void SceneBVH::queryFrustumRecursive(int nodeIndex, const Frustum& frustum, std::vector<entt::entity>& results) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size()))
    {
        return;
    }

    const BVHNode& node = nodes[nodeIndex];

    // Test this node's bounds against the frustum
    if (!frustum.intersectsAABB(node.bounds))
    {
        return;  // Entire subtree is outside frustum
    }

    // If this is a leaf node, add the entity to results
    if (node.isLeaf())
    {
        results.push_back(node.entity);
        return;
    }

    // Recurse on children
    if (node.left_child >= 0)
    {
        queryFrustumRecursive(node.left_child, frustum, results);
    }
    if (node.right_child >= 0)
    {
        queryFrustumRecursive(node.right_child, frustum, results);
    }
}
