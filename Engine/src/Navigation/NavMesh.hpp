#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

// Forward-declare Detour types to avoid including headers in this header
class dtNavMesh;
class dtNavMeshQuery;

namespace Navigation
{

struct NavPath
{
    std::vector<glm::vec3> waypoints;
    bool valid = false;
};

struct NavMeshConfig
{
    // Basic agent parameters (matching Unreal-style UI)
    float max_slope_angle = 45.0f;   // Degrees
    float agent_height    = 1.8f;
    float agent_radius    = 0.3f;

    // Recast voxelization parameters
    float cell_size       = 0.3f;    // XZ voxel size
    float cell_height     = 0.2f;    // Y voxel size
    float max_climb       = 0.5f;    // Max step height agent can climb

    // Mesh simplification
    float max_edge_len           = 12.0f;
    float max_edge_error         = 1.3f;
    int   min_region_area        = 8;
    int   merge_region_area      = 20;
    int   max_verts_per_poly     = 6;
    float detail_sample_dist     = 6.0f;
    float detail_sample_max_error = 1.0f;

    // Legacy (kept for API compat)
    float merge_distance  = 0.001f;
};

// Debug polygon extracted from the Detour navmesh for visualization
struct DebugPoly
{
    std::vector<glm::vec3> vertices;
    glm::vec3 centroid{0.0f};
};

struct ENGINE_API NavMesh
{
    dtNavMesh* dt_navmesh = nullptr;
    dtNavMeshQuery* dt_query = nullptr;

    NavMeshConfig config;
    bool valid = false;

    // Pre-extracted polygon outlines for debug rendering
    std::vector<DebugPoly> debug_polys;

    // Generation stats (populated after generate)
    int total_polys = 0;

    NavMesh();
    ~NavMesh();

    // Move-only
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;
    NavMesh(NavMesh&& other) noexcept;
    NavMesh& operator=(NavMesh&& other) noexcept;

    void clear();
};

} // namespace Navigation
