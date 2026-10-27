#pragma once
#include "Components/Components.hpp"
#include "ECS/Hierarchy.hpp"
#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshGenerator.hpp"
#include "Navigation/NavMeshPathfinder.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Builds a flat 40x40 m ground plane (2 tris, upward winding) in a registry and
// generates a navmesh from it via the real Recast pipeline.
static bool makeGroundNavMesh(Navigation::NavMesh& out, Navigation::NavMeshGenerator::GenerationStats& stats)
{
    static std::vector<vertex> backing = {
        {-20.f, 0.f, -20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 20.f, 0.f,  20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 20.f, 0.f, -20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {-20.f, 0.f, -20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {-20.f, 0.f,  20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 20.f, 0.f,  20.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
    };
    auto ground = std::make_shared<mesh>(backing.data(), backing.size());
    entt::registry reg;
    entt::entity e = reg.create();
    reg.emplace<TransformComponent>(e, TransformComponent(0.f, 0.f, 0.f));
    MeshComponent mc; mc.m_mesh = ground;
    reg.emplace<MeshComponent>(e, mc);
    Navigation::NavMeshConfig cfg;
    cfg.agent_height = 1.8f; cfg.agent_radius = 0.35f; cfg.cell_size = 0.25f;
    cfg.cell_height = 0.2f; cfg.max_climb = 0.5f; cfg.max_edge_len = 2.0f;
    out = Navigation::NavMeshGenerator::generate(reg, cfg, &stats);
    return out.valid && out.total_polys > 0;
}

bool testNavMeshGenerateAndSerializerRoundTrip()
{
    const std::string name = "NavMesh_GenerateSerializerRoundTrip";
    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh mesh;
    if (!makeGroundNavMesh(mesh, stats)) return fail(name, "generate produced no valid mesh");
    if (stats.source_triangles != 2) return fail(name, "source_triangles != 2");
    if (stats.total_polys != mesh.total_polys || mesh.total_polys < 1) return fail(name, "poly count mismatch");
    const auto path = std::filesystem::temp_directory_path() / "garden_navmesh_test_main.nav";
    std::error_code ec; std::filesystem::remove(path, ec);
    if (!Navigation::NavMeshSerializer::save(mesh, path.string())) return fail(name, "save failed");
    Navigation::NavMesh loaded;
    if (!Navigation::NavMeshSerializer::load(loaded, path.string()) || !loaded.valid) return fail(name, "load failed");
    if (loaded.total_polys != mesh.total_polys) return fail(name, "round-trip poly count differs");
    std::filesystem::remove(path, ec);
    // Missing file must fail cleanly (loadBotNavMesh candidate fallback relies on it).
    Navigation::NavMesh missing;
    if (Navigation::NavMeshSerializer::load(missing, path.string())) return fail(name, "load of missing file succeeded");
    return pass(name);
}

bool testNavMeshFindPathTwoPoly()
{
    const std::string name = "NavMesh_FindPathValid";
    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh mesh;
    if (!makeGroundNavMesh(mesh, stats)) return fail(name, "generate produced no valid mesh");
    if (mesh.total_polys < 2) return fail(name, "expected >=2 polys on 40m plane with 2m max edge");
    glm::vec3 nearest(0.f);
    if (!Navigation::NavMeshPathfinder::findNearestPoly(mesh, glm::vec3(-8.f, 0.f, 0.f), nearest)) return fail(name, "findNearestPoly failed at start");
    Navigation::NavPath path = Navigation::NavMeshPathfinder::findPath(mesh, glm::vec3(-8.f, 0.f, 0.f), glm::vec3(8.f, 0.f, 0.f));
    if (!path.valid || path.waypoints.empty()) return fail(name, "findPath invalid across plane");
    if (glm::distance(path.waypoints.back(), glm::vec3(8.f, 0.f, 0.f)) > 1.5f) return fail(name, "path does not end near goal");
    Navigation::NavPath bad = Navigation::NavMeshPathfinder::findPath(mesh, glm::vec3(500.f, 0.f, 500.f), glm::vec3(8.f, 0.f, 0.f));
    if (bad.valid) return fail(name, "findPath from off-mesh point unexpectedly valid");
    return pass(name);
}

static std::shared_ptr<mesh> makeNavMeshTestGround()
{
    static std::vector<vertex> backing = {
        {-10.f, 0.f, -10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 10.f, 0.f,  10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 10.f, 0.f, -10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {-10.f, 0.f, -10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {-10.f, 0.f,  10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        { 10.f, 0.f,  10.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
    };
    return std::make_shared<mesh>(backing.data(), backing.size());
}

static Navigation::NavMeshConfig hierarchyNavMeshConfig()
{
    Navigation::NavMeshConfig cfg;
    cfg.agent_height = 1.8f;
    cfg.agent_radius = 0.2f;
    cfg.cell_size = 0.15f;
    cfg.cell_height = 0.1f;
    cfg.max_climb = 0.5f;
    cfg.max_edge_len = 2.0f;
    return cfg;
}

static glm::vec3 transformNavMeshPoint(const glm::mat4& matrix,
                                       const glm::vec3& point)
{
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

bool testNavMeshHierarchyRootBake()
{
    const std::string name = "NavMesh_HierarchyRootBake";
    entt::registry reg;
    const entt::entity ground_entity = reg.create();
    auto& transform = reg.emplace<TransformComponent>(ground_entity, 37.f, 0.f, -18.f);
    transform.rotation = {0.f, 25.f, 0.f};
    transform.scale = {1.5f, 1.f, 0.75f};
    reg.emplace<MeshComponent>(ground_entity, MeshComponent{makeNavMeshTestGround()});

    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh navmesh = Navigation::NavMeshGenerator::generate(
        reg, hierarchyNavMeshConfig(), &stats);
    if (!navmesh.valid || stats.source_triangles != 2)
        return fail(name, "root geometry was not baked");

    const glm::vec3 world_point = transformNavMeshPoint(
        transform.getTransformMatrix(), glm::vec3(2.f, 0.f, -3.f));
    glm::vec3 nearest{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(navmesh, world_point, nearest))
        return fail(name, "root geometry was not found at its local world pose");
    if (glm::distance(nearest, world_point) > 0.75f)
        return fail(name, "root bake nearest point did not remain in world space");
    return pass(name);
}

bool testNavMeshHierarchyWorldBakeAndPath()
{
    const std::string name = "NavMesh_HierarchyWorldBakeAndPath";
    entt::registry reg;

    const entt::entity parent = reg.create();
    reg.emplace<IdentityComponent>(parent, IdentityComponent{EntityGuid{1, 1}});
    auto& parent_transform = reg.emplace<TransformComponent>(parent, 100.f, 0.f, 50.f);
    parent_transform.rotation = {0.f, 90.f, 0.f};
    parent_transform.scale = {2.f, 1.f, 3.f};

    const entt::entity ground_entity = reg.create();
    reg.emplace<IdentityComponent>(ground_entity, IdentityComponent{EntityGuid{1, 2}});
    auto& child_transform = reg.emplace<TransformComponent>(ground_entity, 5.f, 0.f, -3.f);
    child_transform.rotation = {0.f, 30.f, 0.f};
    reg.emplace<ParentComponent>(ground_entity,
                                 ParentComponent{EntityRef{EntityGuid{1, 1}}});
    reg.emplace<MeshComponent>(ground_entity, MeshComponent{makeNavMeshTestGround()});

    // A separately parented caller supplies the resolved simulation-world
    // point. The child rotation under the ancestor's non-uniform scale makes
    // the composed matrix contain shear, which must remain intact for baking.
    const entt::entity caller = reg.create();
    reg.emplace<TransformComponent>(caller, -7.f, 0.f, -6.f);
    reg.emplace<ParentComponent>(caller,
                                 ParentComponent{EntityRef{EntityGuid{1, 2}}});

    const glm::mat4 ground_world = parent_transform.getTransformMatrix() *
                                   child_transform.getTransformMatrix();
    const glm::vec3 expected_start = transformNavMeshPoint(
        ground_world, glm::vec3(-7.f, 0.f, -6.f));
    const glm::vec3 expected_goal = transformNavMeshPoint(
        ground_world, glm::vec3(7.f, 0.f, 6.f));
    const glm::vec3 caller_world = glm::vec3(
        Hierarchy::computeWorldMatrix(reg, caller)[3]);
    if (glm::distance(caller_world, expected_start) > 0.001f)
        return fail(name, "parented caller did not resolve to its simulation world position");

    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh navmesh = Navigation::NavMeshGenerator::generate(
        reg, hierarchyNavMeshConfig(), &stats);
    if (!navmesh.valid || stats.source_triangles != 2)
        return fail(name, "parented geometry was not baked");

    glm::vec3 nearest{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(navmesh, expected_start, nearest))
        return fail(name, "parented world triangle was not found at its composed pose");
    if (glm::distance(nearest, expected_start) > 0.75f)
        return fail(name, "parented world triangle did not preserve its full matrix pose");

    const Navigation::NavPath path = Navigation::NavMeshPathfinder::findPath(
        navmesh, expected_start, expected_goal);
    if (!path.valid || path.waypoints.empty())
        return fail(name, "world-space path query across parented geometry failed");
    if (glm::distance(path.waypoints.back(), expected_goal) > 0.75f)
        return fail(name, "path output was not in world space");

    glm::vec3 caller_nearest{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(navmesh, caller_world, caller_nearest))
        return fail(name, "resolved parented caller world point was not accepted");
    const glm::vec3 caller_local = reg.get<TransformComponent>(caller).position;
    if (Navigation::NavMeshPathfinder::findNearestPoly(navmesh, caller_local, caller_nearest))
        return fail(name, "local parented caller point unexpectedly queried the world navmesh");
    return pass(name);
}

bool testNavMeshHierarchyStaleParentRootsBake()
{
    const std::string name = "NavMesh_HierarchyStaleParentRootsBake";
    entt::registry reg;
    const entt::entity ground_entity = reg.create();
    auto& transform = reg.emplace<TransformComponent>(ground_entity, 70.f, 0.f, 20.f);
    reg.emplace<ParentComponent>(ground_entity,
                                 ParentComponent{EntityRef{EntityGuid{0xdead, 0xbeef}}});
    reg.emplace<MeshComponent>(ground_entity, MeshComponent{makeNavMeshTestGround()});

    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh navmesh = Navigation::NavMeshGenerator::generate(
        reg, hierarchyNavMeshConfig(), &stats);
    if (!navmesh.valid || stats.source_triangles != 2)
        return fail(name, "stale-parent geometry was not baked as a root");

    glm::vec3 nearest{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(navmesh, transform.position, nearest))
        return fail(name, "stale parent did not use the child local root pose");
    if (glm::distance(nearest, transform.position) > 0.75f)
        return fail(name, "stale parent bake did not remain at the root pose");
    return pass(name);
}
