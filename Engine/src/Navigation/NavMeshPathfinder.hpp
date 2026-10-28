#pragma once

#include "EngineExport.h"
#include "NavMesh.hpp"

namespace Navigation
{

// Captures Detour result details without exposing Detour types to callers.
// The production path query remains bounded; this is for focused diagnosis of
// a route when its returned status needs to distinguish topology from limits.
struct NavPathQueryCapacityStatus
{
    bool failed = false;
    bool partial = false;
    bool buffer_too_small = false;
    bool out_of_nodes = false;
    int corridor_count = 0;
    int corridor_capacity = 0;
    int node_capacity = 0;
};

struct NavPathCapacityDiagnostic
{
    bool endpoints_projected = false;
    bool expanded_query_initialized = false;
    NavPathQueryCapacityStatus current;
    NavPathQueryCapacityStatus expanded;
};

class ENGINE_API NavMeshPathfinder
{
public:
    static NavPath findPath(const NavMesh& navmesh,
                            const glm::vec3& start,
                            const glm::vec3& goal);

    // Find the nearest polygon to a world-space point
    // Returns true if a valid polygon was found
    static bool findNearestPoly(const NavMesh& navmesh, const glm::vec3& point,
                                glm::vec3& nearest_point);

    // Runs the same projected route through the production query and an
    // isolated, higher-capacity diagnostic query. It never changes the
    // NavMesh-owned production query or its configured limits.
    static NavPathCapacityDiagnostic diagnosePathCapacity(
        const NavMesh& navmesh, const glm::vec3& start, const glm::vec3& goal);
};

} // namespace Navigation
