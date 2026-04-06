#pragma once

#include "EngineExport.h"
#include "NavMesh.hpp"

namespace Navigation
{

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
};

} // namespace Navigation
