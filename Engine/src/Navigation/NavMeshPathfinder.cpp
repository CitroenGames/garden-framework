#include "NavMeshPathfinder.hpp"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

namespace Navigation
{

static const float SEARCH_EXTENTS[3] = { 2.0f, 4.0f, 2.0f };

NavPath NavMeshPathfinder::findPath(const NavMesh& navmesh,
                                     const glm::vec3& start,
                                     const glm::vec3& goal)
{
    NavPath result;

    if (!navmesh.valid || !navmesh.dt_query)
        return result;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);

    // Find nearest polygons to start and goal
    dtPolyRef startRef = 0, goalRef = 0;
    float nearestStart[3], nearestGoal[3];

    navmesh.dt_query->findNearestPoly(&start.x, SEARCH_EXTENTS, &filter, &startRef, nearestStart);
    navmesh.dt_query->findNearestPoly(&goal.x, SEARCH_EXTENTS, &filter, &goalRef, nearestGoal);

    if (!startRef || !goalRef)
        return result;

    // Find polygon corridor
    static const int MAX_POLYS = 256;
    dtPolyRef polys[MAX_POLYS];
    int polyCount = 0;

    dtStatus status = navmesh.dt_query->findPath(startRef, goalRef,
                                                  &start.x, &goal.x,
                                                  &filter, polys, &polyCount, MAX_POLYS);
    if (dtStatusFailed(status) || polyCount == 0)
        return result;

    // Find straight path (string-pulled waypoints)
    float straightPath[MAX_POLYS * 3];
    int straightPathCount = 0;

    navmesh.dt_query->findStraightPath(&start.x, &goal.x,
                                        polys, polyCount,
                                        straightPath, nullptr, nullptr,
                                        &straightPathCount, MAX_POLYS);

    if (straightPathCount == 0)
        return result;

    result.waypoints.reserve(straightPathCount);
    for (int i = 0; i < straightPathCount; i++)
    {
        result.waypoints.push_back(glm::vec3(
            straightPath[i * 3 + 0],
            straightPath[i * 3 + 1],
            straightPath[i * 3 + 2]));
    }

    result.valid = !result.waypoints.empty();
    return result;
}

bool NavMeshPathfinder::findNearestPoly(const NavMesh& navmesh, const glm::vec3& point,
                                         glm::vec3& nearest_point)
{
    if (!navmesh.valid || !navmesh.dt_query)
        return false;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);

    dtPolyRef ref = 0;
    float nearest[3];

    navmesh.dt_query->findNearestPoly(&point.x, SEARCH_EXTENTS, &filter, &ref, nearest);

    if (!ref)
        return false;

    nearest_point = glm::vec3(nearest[0], nearest[1], nearest[2]);
    return true;
}

} // namespace Navigation
