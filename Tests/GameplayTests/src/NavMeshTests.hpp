#pragma once
#include "Components/Components.hpp"
#include "ECS/Hierarchy.hpp"
#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshGenerator.hpp"
#include "Navigation/NavMeshPathfinder.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "Project/ProjectManager.hpp"
#include "shared/BotNavigationRuntime.hpp"
#include "shared/BotRouteData.hpp"
#include "shared/CompetitiveLogic.hpp"
#include "shared/CompetitiveMapData.hpp"
#include "shared/SelectedLevelSidecars.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

static void appendFlatNavQuad(std::vector<vertex>& backing, float min_x, float max_x,
                              float min_z, float max_z)
{
    const auto add = [&](float x, float z) {
        backing.push_back({x, 0.f, z, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f});
    };
    add(min_x, min_z); add(max_x, max_z); add(max_x, min_z);
    add(min_x, min_z); add(min_x, max_z); add(max_x, max_z);
}

static Navigation::NavMeshConfig navigationAdmissionFixtureConfig()
{
    Navigation::NavMeshConfig cfg;
    cfg.agent_height = 1.8f; cfg.agent_radius = 0.35f; cfg.cell_size = 0.25f;
    cfg.cell_height = 0.2f; cfg.max_climb = 0.5f; cfg.max_edge_len = 2.0f;
    return cfg;
}

// Two projected islands with an eight metre gap.  Detour must distinguish
// this topology miss from a production query capacity limit.
static bool makeDisconnectedAdmissionNavMesh(
    Navigation::NavMesh& out, Navigation::NavMeshGenerator::GenerationStats& stats)
{
    std::vector<vertex> backing;
    appendFlatNavQuad(backing, -16.f, -4.f, -4.f, 4.f);
    appendFlatNavQuad(backing, 4.f, 16.f, -4.f, 4.f);
    auto ground = std::make_shared<mesh>(backing.data(), backing.size());
    entt::registry reg;
    const entt::entity entity = reg.create();
    reg.emplace<TransformComponent>(entity, TransformComponent(0.f, 0.f, 0.f));
    reg.emplace<MeshComponent>(entity, MeshComponent{ground});
    out = Navigation::NavMeshGenerator::generate(reg, navigationAdmissionFixtureConfig(), &stats);
    return out.valid && out.total_polys > 1;
}

// A 600 m, two metre-wide corridor forces the bounded 256-poly production
// query to exhaust while the diagnostic query can complete with its expanded
// corridor and node capacities.
static bool makeCapacityAdmissionNavMesh(
    Navigation::NavMesh& out, Navigation::NavMeshGenerator::GenerationStats& stats)
{
    std::vector<vertex> backing;
    backing.reserve(300u * 6u);
    for (int segment = 0; segment < 300; ++segment) {
        const float min_x = -300.f + static_cast<float>(segment) * 2.f;
        appendFlatNavQuad(backing, min_x, min_x + 2.f, -1.f, 1.f);
    }
    auto ground = std::make_shared<mesh>(backing.data(), backing.size());
    entt::registry reg;
    const entt::entity entity = reg.create();
    reg.emplace<TransformComponent>(entity, TransformComponent(0.f, 0.f, 0.f));
    reg.emplace<MeshComponent>(entity, MeshComponent{ground});
    out = Navigation::NavMeshGenerator::generate(reg, navigationAdmissionFixtureConfig(), &stats);
    return out.valid && out.total_polys > 256;
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

bool testBotNavigationAdmissionClassification()
{
    const std::string name = "Bot_NavigationAdmissionClassification";
    using namespace Bots;

    Navigation::NavMeshGenerator::GenerationStats ground_stats;
    Navigation::NavMesh ground;
    if (!makeGroundNavMesh(ground, ground_stats))
        return fail(name, "ground fixture generation failed");
    glm::vec3 projected{};
    const bool start_projected = Navigation::NavMeshPathfinder::findNearestPoly(
        ground, glm::vec3(-8.f, 0.f, 0.f), projected);
    const bool goal_projected = Navigation::NavMeshPathfinder::findNearestPoly(
        ground, glm::vec3(8.f, 0.f, 0.f), projected);
    const bool off_mesh = Navigation::NavMeshPathfinder::findNearestPoly(
        ground, glm::vec3(500.f, 0.f, 500.f), projected);
    if (!start_projected || !goal_projected || off_mesh)
        return fail(name, "endpoint projection fixture did not separate mesh and off-mesh points");
    if (classifyNavAdmission(false, goal_projected, false, false, true, false).category !=
            NavFailureCategory::StartOffMesh ||
        classifyNavAdmission(false, false, false, false, true, false).category !=
            NavFailureCategory::StartOffMesh ||
        classifyNavAdmission(start_projected, false, false, false, true, false).category !=
            NavFailureCategory::GoalOffMesh)
        return fail(name, "endpoint projection categories or both-off-mesh precedence changed");

    Navigation::NavMeshGenerator::GenerationStats disconnected_stats;
    Navigation::NavMesh disconnected;
    if (!makeDisconnectedAdmissionNavMesh(disconnected, disconnected_stats))
        return fail(name, "disconnected topology fixture generation failed");
    glm::vec3 disconnected_start{};
    glm::vec3 disconnected_goal{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(disconnected, glm::vec3(-10.f, 0.f, 0.f), disconnected_start) ||
        !Navigation::NavMeshPathfinder::findNearestPoly(disconnected, glm::vec3(10.f, 0.f, 0.f), disconnected_goal))
        return fail(name, "disconnected endpoints did not project");
    const Navigation::NavPath disconnected_path = Navigation::NavMeshPathfinder::findPath(
        disconnected, disconnected_start, disconnected_goal);
    if (disconnected_path.valid || disconnected_path.query_capacity_exhausted ||
        classifyNavAdmission(true, true, false, disconnected_path.query_capacity_exhausted,
                             false, false).category != NavFailureCategory::NoPath)
        return fail(name, "topology NoPath was mislabeled during the capacity cooldown");

    Navigation::NavMeshGenerator::GenerationStats capacity_stats;
    Navigation::NavMesh capacity;
    if (!makeCapacityAdmissionNavMesh(capacity, capacity_stats))
        return fail(name, "capacity fixture did not exceed production corridor capacity");
    glm::vec3 capacity_start{};
    glm::vec3 capacity_goal{};
    if (!Navigation::NavMeshPathfinder::findNearestPoly(capacity, glm::vec3(-299.f, 0.f, 0.f), capacity_start) ||
        !Navigation::NavMeshPathfinder::findNearestPoly(capacity, glm::vec3(299.f, 0.f, 0.f), capacity_goal))
        return fail(name, "capacity endpoints did not project");
    const Navigation::NavPath capacity_path = Navigation::NavMeshPathfinder::findPath(
        capacity, capacity_start, capacity_goal);
    if (capacity_path.valid || !capacity_path.query_capacity_exhausted)
        return fail(name, "long corridor did not report production capacity exhaustion");
    const NavAdmissionClassification awaiting_capacity = classifyNavAdmission(
        true, true, false, capacity_path.query_capacity_exhausted, true, false);
    if (awaiting_capacity.category != NavFailureCategory::NoPath || !awaiting_capacity.inspect_capacity)
        return fail(name, "production capacity signal did not request bounded confirmation");
    const Navigation::NavPathCapacityDiagnostic diagnostic =
        Navigation::NavMeshPathfinder::diagnosePathCapacity(capacity, capacity_start, capacity_goal);
    const bool capacity_confirmed = diagnostic.endpoints_projected &&
        (diagnostic.current.buffer_too_small || diagnostic.current.out_of_nodes) &&
        diagnostic.expanded_query_initialized && !diagnostic.expanded.failed &&
        !diagnostic.expanded.partial && !diagnostic.expanded.buffer_too_small &&
        !diagnostic.expanded.out_of_nodes && diagnostic.expanded.corridor_count > 0;
    if (!capacity_confirmed || classifyNavAdmission(true, true, false, true, true, true).category !=
            NavFailureCategory::QueryCapacity)
        return fail(name, "expanded capacity confirmation did not produce QueryCapacity");
    const NavAdmissionClassification suppressed_capacity = classifyNavAdmission(
        true, true, false, true, false, false);
    if (suppressed_capacity.category != NavFailureCategory::QueryCapacity ||
        !suppressed_capacity.capacity_inspection_suppressed)
        return fail(name, "evidenced capacity did not retain its suppressed category");
    return pass(name);
}

bool testBotNavWorldResourceLifecycle()
{
    const std::string name = "Bot_NavWorldResourceLifecycle";
    Navigation::NavMeshGenerator::GenerationStats stats;
    Navigation::NavMesh mesh;
    if (!makeGroundNavMesh(mesh, stats)) return fail(name, "fixture navmesh generation failed");

    const auto directory = std::filesystem::temp_directory_path() / "garden_bot_nav_runtime";
    const auto first_path = directory / "first.nav";
    const auto second_path = directory / "second.nav";
    const auto corrupt_path = directory / "corrupt.nav";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    if (ec) return fail(name, "could not create fixture directory");
    if (!Navigation::NavMeshSerializer::save(mesh, first_path.string()) ||
        !Navigation::NavMeshSerializer::save(mesh, second_path.string())) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "could not save fixture navmeshes");
    }

    // Separate registry resources must own independent Detour allocations and
    // state. Loading one must not make the other valid, and reset must leave
    // no stale path/mesh for a later level.
    Bots::BotWorldRuntime first_world;
    Bots::BotWorldRuntime second_world;
    if (!first_world.loadResolvedNavMesh(first_path.string()) ||
        !first_world.hasValidNavMesh() ||
        first_world.nav_status != Bots::NavMeshLoadStatus::Loaded ||
        first_world.nav_format_version != Navigation::NavMeshSerializer::kCurrentVersion ||
        second_world.hasValidNavMesh()) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "first world load leaked into second world");
    }
    first_world.target_count = 4;
    first_world.input_tick = 42;
    first_world.reset();
    if (first_world.hasValidNavMesh() || !first_world.resolved_nav_path.empty() ||
        first_world.target_count != 0 || first_world.input_tick != 1 ||
        first_world.nav_format_version != 0 ||
        first_world.nav_status != Bots::NavMeshLoadStatus::NotRequested) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "reset retained level-owned bot state");
    }

    // A missing asset and a malformed existing asset must both clear a prior
    // good mesh, but diagnostics must distinguish the operator action needed.
    if (!first_world.loadResolvedNavMesh(first_path.string()) ||
        first_world.loadResolvedNavMesh((directory / "missing.nav").string()) ||
        first_world.hasValidNavMesh() ||
        first_world.nav_status != Bots::NavMeshLoadStatus::Missing) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "missing asset did not safely disable bots");
    }
    {
        std::ofstream corrupt(corrupt_path, std::ios::binary);
        corrupt << "not a navmesh";
    }
    if (first_world.loadResolvedNavMesh(corrupt_path.string()) ||
        first_world.hasValidNavMesh() ||
        first_world.nav_status != Bots::NavMeshLoadStatus::Invalid) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "corrupt asset did not safely disable bots");
    }
    if (!second_world.loadResolvedNavMesh(second_path.string()) ||
        !second_world.hasValidNavMesh() ||
        second_world.resolved_nav_path != second_path.string()) {
        std::filesystem::remove_all(directory, ec);
        return fail(name, "second world could not load independently");
    }

    std::filesystem::remove_all(directory, ec);
    return pass(name);
}

bool testBotNavigationDiagnosticWorldIsolation()
{
    const std::string name = "Bot_NavigationDiagnosticWorldIsolation";
    Bots::BotWorldRuntime first;
    Bots::BotWorldRuntime second;

    // Append order remains stable across wrap and retains no route position,
    // waypoint, target, or entity identity in the per-world record.
    for (uint32_t tick = 1; tick <= 65; ++tick) {
        const Bots::NavFailureCategory category = (tick & 1u)
            ? Bots::NavFailureCategory::NoPath : Bots::NavFailureCategory::LocalBlock;
        first.recordNavigationDiagnostic(tick, 7, category,
            category == Bots::NavFailureCategory::LocalBlock
                ? Bots::LocalBlockDetail::Static : Bots::LocalBlockDetail::Crowd,
            tick / 4u);
    }
    if (first.navigation_diagnostics.size() != Bots::NavigationDiagnosticRing::kCapacity ||
        first.navigation_diagnostics.oldest().tick != 2 ||
        first.navigation_diagnostics.newest().tick != 65 ||
        first.navigation_diagnostics.at(0).detail != Bots::LocalBlockDetail::Static ||
        first.navigation_diagnostics.at(1).detail != Bots::LocalBlockDetail::None)
        return fail(name, "diagnostic ring did not retain stable bounded records");

    // Capacity inspection gates belong to a world. A suppressed new episode
    // increments only that world's counter, then opens after 60 fixed ticks.
    if (!first.beginCapacityDiagnostic(10) || first.beginCapacityDiagnostic(11) ||
        first.capacity_diagnostic_calls != 1 || first.capacity_diagnostic_suppressions != 1 ||
        !first.beginCapacityDiagnostic(70))
        return fail(name, "capacity diagnostic gate did not rate limit at fixed ticks");
    if (!second.beginCapacityDiagnostic(11) || second.capacity_diagnostic_calls != 1 ||
        second.capacity_diagnostic_suppressions != 0)
        return fail(name, "capacity diagnostic state leaked between worlds");

    const std::string snapshot = first.formatNavigationDiagnosticSnapshot();
    for (const char* field : {"[BotDebug] navigation_diagnostics", "count=64", "recoveries=0",
                              "capacity_calls=2", "capacity_suppressions=1", "last_tick=65",
                              "last_bot=7", "last_category=no_path", "last_detail=none",
                              "last_recovery=16", "last_capacity_suppressed=0"}) {
        if (snapshot.find(field) == std::string::npos)
            return fail(name, std::string("navigation snapshot missing field: ") + field);
    }
    for (const char* forbidden : {"pos=", "path=", "target=", "hit=", "entity=", "peer="}) {
        if (snapshot.find(forbidden) != std::string::npos)
            return fail(name, std::string("navigation snapshot leaked field: ") + forbidden);
    }

    first.navigation_recoveries = 3;
    first.reset();
    if (first.navigation_diagnostics.size() != 0 || first.capacity_diagnostic_calls != 0 ||
        first.capacity_diagnostic_suppressions != 0 || first.navigation_recoveries != 0 ||
        !first.beginCapacityDiagnostic(0))
        return fail(name, "world reset retained navigation diagnostic state");
    if (second.navigation_diagnostics.size() != 0 || second.capacity_diagnostic_calls != 1)
        return fail(name, "first world reset changed second world diagnostic state");
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

static std::filesystem::path findFpSShooterProjectDescriptor()
{
    // GameplayTests can be launched from the checkout root or bin/. The
    // selected-level contract begins with the project descriptor, not a
    // hardcoded content stem or a process-working-directory sidecar probe.
    constexpr std::array<const char*, 3> candidates = {
        "Templates/FPSShooter/FPSShooter.garden",
        "../Templates/FPSShooter/FPSShooter.garden",
        "../../Templates/FPSShooter/FPSShooter.garden",
    };
    for (const char* candidate : candidates) {
        const std::filesystem::path path(candidate);
        if (std::filesystem::is_regular_file(path))
            return path;
    }
    return {};
}

class ScopedNavTestAssetRoot
{
public:
    explicit ScopedNavTestAssetRoot(const std::filesystem::path& root)
        : m_assets(Assets::AssetManager::get()), m_previous_root(m_assets.getAssetRoot())
    {
        m_assets.setAssetRoot(root.string());
    }

    ~ScopedNavTestAssetRoot()
    {
        m_assets.setAssetRoot(m_previous_root);
    }

private:
    Assets::AssetManager& m_assets;
    std::string m_previous_root;
};

static std::string describeNavRouteCapacity(
    const Navigation::NavPathCapacityDiagnostic& diagnostic)
{
    if (!diagnostic.endpoints_projected)
        return "diagnostic=nearest-poly-failed";
    if (!diagnostic.expanded_query_initialized)
        return "diagnostic=temporary-query-init-failed";

    const auto describeQuery = [](const Navigation::NavPathQueryCapacityStatus& status) {
        std::string details;
        const auto append = [&details](const char* detail) {
            if (!details.empty())
                details += "|";
            details += detail;
        };
        if (status.failed) append("failure");
        if (!status.failed) append("success");
        if (status.partial) append("partial");
        if (status.buffer_too_small) append("buffer-too-small");
        if (status.out_of_nodes) append("out-of-nodes");
        return details + ", polyCount=" + std::to_string(status.corridor_count) +
            ", corridorCap=" + std::to_string(status.corridor_capacity) +
            ", nodeCap=" + std::to_string(status.node_capacity);
    };

    const auto& current = diagnostic.current;
    std::string classification;
    if (current.out_of_nodes)
        classification = "node-pool-exhausted";
    else if (current.buffer_too_small)
        classification = "output-buffer-bounded";
    else if (current.partial)
        classification = "partial";
    else
        classification = "no-current-capacity-detail";
    return "diagnostic=" + classification + " current(" + describeQuery(current) +
        ") expanded(" + describeQuery(diagnostic.expanded) + ")";
}

bool testNavMeshSelectedLevelRouteMatrix()
{
    const std::string name = "NavMesh_SelectedLevelRouteMatrix";
    const std::filesystem::path project_path = findFpSShooterProjectDescriptor();
    if (project_path.empty())
        return fail(name, "FPSShooter project descriptor was not found");

    ProjectManager project;
    if (!project.loadProject(project_path.string()))
        return fail(name, "could not load FPSShooter project descriptor");
    const auto selected_level = ProjectManager::normalizeSelectedLevelArtifactPath(
        project.getDescriptor().default_level);
    if (!selected_level)
        return fail(name, "project default level is not a normalized source or packaged level artifact");

    // Same-stem resolution must use the project AssetManager root, exactly as
    // the module does.  Scope this state so the test cannot leak its source
    // project root into later fixtures.
    ScopedNavTestAssetRoot asset_root(
        std::filesystem::path(project.getProjectRoot()) / "assets");
    FpsContent::SelectedLevelSidecars sidecars;
    std::string sidecar_error;
    if (!FpsContent::resolveSelectedLevelSidecars(*selected_level, sidecars, &sidecar_error))
        return fail(name, "selected level sidecars could not be resolved: " + sidecar_error);

    const std::filesystem::path nav_path = sidecars.nav_path;
    if (!std::filesystem::is_regular_file(nav_path))
        return fail(name, "selected nav artifact is missing: " + sidecars.nav_logical_path);

    std::ifstream nav_file(nav_path, std::ios::binary);
    uint32_t nav_version = 0;
    uint32_t nav_magic = 0;
    nav_file.read(reinterpret_cast<char*>(&nav_magic), sizeof(nav_magic));
    nav_file.read(reinterpret_cast<char*>(&nav_version), sizeof(nav_version));
    if (!nav_file || nav_version != Navigation::NavMeshSerializer::kCurrentVersion)
        return fail(name, "selected nav artifact has an unsupported format version: " +
            sidecars.nav_logical_path);

    Bots::BotWorldRuntime runtime;
    if (!runtime.loadResolvedNavMesh(nav_path.string()) ||
        !runtime.hasValidNavMesh() ||
        runtime.nav_format_version != Navigation::NavMeshSerializer::kCurrentVersion ||
        runtime.navmesh.total_polys <= 0) {
        return fail(name, "selected nav artifact was not current and non-empty: " +
            sidecars.nav_logical_path);
    }

    std::ifstream competitive_file(sidecars.competitive_path, std::ios::binary);
    if (!competitive_file)
        return fail(name, "selected competitive marker sidecar is unreadable: " +
            sidecars.competitive_logical_path);
    const std::string competitive_text((std::istreambuf_iterator<char>(competitive_file)),
                                       std::istreambuf_iterator<char>());
    FpsContent::CompetitiveMapData competitive_data;
    std::string competitive_error;
    if (!FpsContent::parseCompetitiveMapData(competitive_text, competitive_data, &competitive_error))
        return fail(name, "selected competitive marker sidecar was rejected: " +
            competitive_error);

    std::ifstream sites_file(sidecars.sites_path, std::ios::binary);
    if (!sites_file)
        return fail(name, "selected bomb-site sidecar is unreadable: " + sidecars.sites_logical_path);
    const std::string sites_text((std::istreambuf_iterator<char>(sites_file)),
                                 std::istreambuf_iterator<char>());
    std::vector<Competitive::BombSite> sites;
    std::string sites_error;
    if (!Competitive::parseAuthoredBombSites(sites_text, sites, &sites_error) || sites.size() != 2)
        return fail(name, "selected authored A/B sites were rejected: " + sites_error);

    std::ifstream routes_file(sidecars.ai_path, std::ios::binary);
    if (!routes_file)
        return fail(name, "selected bot route sidecar is unreadable: " + sidecars.ai_logical_path);
    const std::string routes_text((std::istreambuf_iterator<char>(routes_file)),
                                  std::istreambuf_iterator<char>());
    Bots::AuthoredRouteData route_data;
    std::string routes_error;
    if (!Bots::parseAuthoredRouteData(routes_text, route_data, &routes_error))
        return fail(name, "selected bot route sidecar was rejected: " + routes_error);
    if (!Bots::authoredRouteLevelMatchesSelectedStem(route_data, sidecars.stem))
        return fail(name, "selected bot route sidecar declares a different level stem: " +
            route_data.level + " for " + sidecars.stem);
    if (route_data.routes.empty() || route_data.holds.empty())
        return fail(name, "selected bot route sidecar requires attacker routes and defender holds");

    const auto describePoint = [](const glm::vec3& point) {
        return "(" + std::to_string(point.x) + ", " + std::to_string(point.y) +
            ", " + std::to_string(point.z) + ")";
    };

    // A complete result is required for every route.  The detailed diagnostic
    // retains the former capacity evidence so a failure distinguishes missing
    // topology from a production query capacity limit.
    const auto verifyRoute = [&](std::string_view route_kind, std::string_view from_id,
                                 const glm::vec3& from, std::string_view to_id,
                                 const glm::vec3& to) {
        glm::vec3 projected_from{};
        glm::vec3 projected_to{};
        if (!Navigation::NavMeshPathfinder::findNearestPoly(runtime.navmesh, from, projected_from) ||
            !Navigation::NavMeshPathfinder::findNearestPoly(runtime.navmesh, to, projected_to)) {
            return fail(name, "route=" + std::string(route_kind) + " from=" + std::string(from_id) +
                " rawFrom=" + describePoint(from) + " to=" + std::string(to_id) +
                " rawTo=" + describePoint(to) + " did not project to " + sidecars.nav_logical_path);
        }

        const Navigation::NavPath path = Navigation::NavMeshPathfinder::findPath(
            runtime.navmesh, projected_from, projected_to);
        if (!path.valid || path.waypoints.empty()) {
            const Navigation::NavPathCapacityDiagnostic diagnostic =
                Navigation::NavMeshPathfinder::diagnosePathCapacity(
                    runtime.navmesh, projected_from, projected_to);
            return fail(name, "route=" + std::string(route_kind) + " from=" + std::string(from_id) +
                " projectedFrom=" + describePoint(projected_from) + " to=" + std::string(to_id) +
                " projectedTo=" + describePoint(projected_to) + " " +
                describeNavRouteCapacity(diagnostic));
        }

        const float endpoint_distance = glm::distance(path.waypoints.back(), projected_to);
        if (endpoint_distance > 0.5f) {
            return fail(name, "route endpoint miss route=" + std::string(route_kind) +
                " from=" + std::string(from_id) + " projectedFrom=" +
                describePoint(projected_from) + " to=" + std::string(to_id) +
                " projectedTo=" + describePoint(projected_to) + " end=" +
                describePoint(path.waypoints.back()) + " distance=" +
                std::to_string(endpoint_distance));
        }
        return true;
    };

    for (const FpsContent::CompetitiveTeamSpawn& spawn : competitive_data.team_spawns) {
        for (const Competitive::BombSite& site : sites) {
            if (!verifyRoute("team-spawn-to-site", spawn.id, spawn.position,
                             "site-" + std::to_string(site.site_index), site.centre))
                return false;
        }
        for (const FpsContent::CompetitiveBuyZone& buy_zone : competitive_data.buy_zones) {
            if (buy_zone.team != spawn.team)
                continue;
            if (!verifyRoute("team-spawn-to-own-buy", spawn.id, spawn.position,
                             buy_zone.id, buy_zone.centre))
                return false;
        }
    }

    for (const FpsContent::CompetitiveBuyZone& buy_zone : competitive_data.buy_zones) {
        for (const Competitive::BombSite& site : sites) {
            if (!verifyRoute("buy-to-site", buy_zone.id, buy_zone.centre,
                             "site-" + std::to_string(site.site_index), site.centre))
                return false;
        }
    }

    for (size_t from_index = 0; from_index < sites.size(); ++from_index) {
        for (size_t to_index = 0; to_index < sites.size(); ++to_index) {
            if (from_index == to_index)
                continue;
            if (!verifyRoute("site-to-site", "site-" + std::to_string(sites[from_index].site_index),
                             sites[from_index].centre,
                             "site-" + std::to_string(sites[to_index].site_index),
                             sites[to_index].centre))
                return false;
        }
    }

    const auto siteForIndex = [&](uint8_t index) -> const Competitive::BombSite* {
        for (const Competitive::BombSite& site : sites)
            if (site.site_index == index)
                return &site;
        return nullptr;
    };
    const auto verifyTacticalTarget = [&](std::string_view kind, std::string_view id,
                                          Bots::RouteTeam route_team, uint8_t site_index,
                                          const glm::vec3& target) {
        if (site_index == Bots::kAnyRouteSite)
            return fail(name, "selected tactical " + std::string(kind) + "=" + std::string(id) +
                " lacks an authored bomb-site index");
        const Competitive::BombSite* site = siteForIndex(site_index);
        if (site == nullptr)
            return fail(name, "selected tactical " + std::string(kind) + "=" + std::string(id) +
                " references an unknown bomb-site index");
        for (const FpsContent::CompetitiveTeamSpawn& spawn : competitive_data.team_spawns) {
            if (!Bots::routeTeamMatches(route_team, spawn.team))
                continue;
            if (!verifyRoute("team-spawn-to-tactical", spawn.id, spawn.position, id, target) ||
                !verifyRoute("tactical-to-site", id, target,
                             "site-" + std::to_string(site->site_index), site->centre))
                return false;
        }
        return true;
    };

    // Runtime selection uses ordinary nav queries from the pawn to these
    // route/hold targets. Verify every target has a complete route from each
    // eligible spawn and onward to its declared site; target selection cannot
    // introduce a direct-through-geometry fallback.
    for (const Bots::AuthoredRoute& route : route_data.routes) {
        if (!verifyTacticalTarget("route", route.id, route.team, route.site_index,
                                  route.points.back()))
            return false;
    }
    for (const Bots::AuthoredHold& hold : route_data.holds) {
        if (!verifyTacticalTarget("hold", hold.id, hold.team, hold.site_index, hold.point))
            return false;
    }

    // Attackers select routes and defenders select holds in ServerModule.
    // Exercise the same deterministic selector and prove each selected target
    // remains reachable from every spawn on that team.
    const auto verifySelectedTargets = [&](Competitive::FpsTeam team, bool prefer_hold) {
        for (const Competitive::BombSite& site : sites) {
            for (int32_t bot_index = 0; bot_index < 8; ++bot_index) {
                glm::vec3 target(0.0f);
                if (!Bots::selectAuthoredRouteTarget(route_data, team, site.site_index,
                                                     bot_index, prefer_hold, target))
                    return fail(name, "no selected tactical target for team/site pair");
                for (const FpsContent::CompetitiveTeamSpawn& spawn : competitive_data.team_spawns) {
                    if (spawn.team != team)
                        continue;
                    if (!verifyRoute("selected-tactical", spawn.id, spawn.position,
                                     "bot-" + std::to_string(bot_index), target))
                        return false;
                }
            }
        }
        return true;
    };
    if (!verifySelectedTargets(Competitive::FpsTeam::Attackers, false) ||
        !verifySelectedTargets(Competitive::FpsTeam::Defenders, true))
        return false;

    return pass(name);
}
