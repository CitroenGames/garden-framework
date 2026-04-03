#pragma once

#include "NavMesh.hpp"

namespace Navigation
{

class NavMeshPathfinder
{
public:
    static NavPath findPath(const NavMesh& navmesh,
                            const glm::vec3& start,
                            const glm::vec3& goal);

    // Find which triangle contains a world-space point (XZ projection + closest Y)
    static int32_t findContainingTriangle(const NavMesh& navmesh, const glm::vec3& point);

private:
    static std::vector<int32_t> aStarSearch(const NavMesh& navmesh,
                                            int32_t start_tri,
                                            int32_t goal_tri);

    static std::vector<Portal> getPortals(const NavMesh& navmesh,
                                          const std::vector<int32_t>& corridor,
                                          const glm::vec3& start,
                                          const glm::vec3& goal);

    static std::vector<glm::vec3> funnelSmooth(const std::vector<Portal>& portals,
                                                const glm::vec3& start,
                                                const glm::vec3& goal);
};

} // namespace Navigation
