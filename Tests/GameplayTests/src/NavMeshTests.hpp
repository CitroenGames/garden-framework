#pragma once
#include "Components/Components.hpp"
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
