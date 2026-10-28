#include "NavMeshPathfinder.hpp"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <algorithm>
#include <vector>

namespace Navigation
{

static const float SEARCH_EXTENTS[3] = { 2.0f, 4.0f, 2.0f };
static constexpr int PATH_MAX_POLYS = 256;
static constexpr int QUERY_NODE_CAPACITY = 2048;
// Detour stores node indices in an unsigned short. Its documented maximum is
// therefore 65535; the diagnostic must remain inside that independent limit.
static constexpr int DIAGNOSTIC_MAX_QUERY_NODES = 65535;
static constexpr unsigned int INCOMPLETE_PATH_STATUS_DETAILS =
    DT_PARTIAL_RESULT | DT_BUFFER_TOO_SMALL | DT_OUT_OF_NODES;

static bool isCompletePathStatus(const dtStatus status)
{
    return !dtStatusFailed(status) &&
        !dtStatusDetail(status, INCOMPLETE_PATH_STATUS_DETAILS);
}

static void appendPathStatus(NavPathQueryCapacityStatus& result, const dtStatus status)
{
    result.failed = result.failed || dtStatusFailed(status);
    result.partial = result.partial || dtStatusDetail(status, DT_PARTIAL_RESULT);
    result.buffer_too_small = result.buffer_too_small ||
        dtStatusDetail(status, DT_BUFFER_TOO_SMALL);
    result.out_of_nodes = result.out_of_nodes || dtStatusDetail(status, DT_OUT_OF_NODES);
}

static NavPathQueryCapacityStatus inspectPathQuery(
    dtNavMeshQuery& query, const dtPolyRef start_ref, const dtPolyRef goal_ref,
    const glm::vec3& start, const glm::vec3& goal, const dtQueryFilter& filter,
    const int corridor_capacity, const int node_capacity)
{
    NavPathQueryCapacityStatus result;
    result.corridor_capacity = corridor_capacity;
    result.node_capacity = node_capacity;

    std::vector<dtPolyRef> corridor(static_cast<size_t>(corridor_capacity));
    const dtStatus corridor_status = query.findPath(
        start_ref, goal_ref, &start.x, &goal.x, &filter, corridor.data(),
        &result.corridor_count, corridor_capacity);
    appendPathStatus(result, corridor_status);
    if (dtStatusFailed(corridor_status) || result.corridor_count == 0)
        return result;

    std::vector<float> straight_path(static_cast<size_t>(corridor_capacity) * 3);
    int straight_path_count = 0;
    const dtStatus straight_status = query.findStraightPath(
        &start.x, &goal.x, corridor.data(), result.corridor_count,
        straight_path.data(), nullptr, nullptr, &straight_path_count, corridor_capacity);
    appendPathStatus(result, straight_status);
    return result;
}

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
    dtPolyRef polys[PATH_MAX_POLYS];
    int polyCount = 0;

    dtStatus status = navmesh.dt_query->findPath(startRef, goalRef,
                                                  &start.x, &goal.x,
                                                  &filter, polys, &polyCount, PATH_MAX_POLYS);
    if (!isCompletePathStatus(status) || polyCount == 0)
        return result;

    // Find straight path (string-pulled waypoints)
    float straightPath[PATH_MAX_POLYS * 3];
    int straightPathCount = 0;

    const dtStatus straightPathStatus = navmesh.dt_query->findStraightPath(
        &start.x, &goal.x, polys, polyCount, straightPath, nullptr, nullptr,
        &straightPathCount, PATH_MAX_POLYS);

    if (!isCompletePathStatus(straightPathStatus) || straightPathCount == 0)
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

NavPathCapacityDiagnostic NavMeshPathfinder::diagnosePathCapacity(
    const NavMesh& navmesh, const glm::vec3& start, const glm::vec3& goal)
{
    NavPathCapacityDiagnostic diagnostic;
    diagnostic.current.corridor_capacity = PATH_MAX_POLYS;
    diagnostic.current.node_capacity = QUERY_NODE_CAPACITY;
    if (!navmesh.valid || !navmesh.dt_navmesh || !navmesh.dt_query || navmesh.total_polys <= 0)
        return diagnostic;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0);

    dtPolyRef start_ref = 0;
    dtPolyRef goal_ref = 0;
    float nearest_start[3]{};
    float nearest_goal[3]{};
    navmesh.dt_query->findNearestPoly(
        &start.x, SEARCH_EXTENTS, &filter, &start_ref, nearest_start);
    navmesh.dt_query->findNearestPoly(
        &goal.x, SEARCH_EXTENTS, &filter, &goal_ref, nearest_goal);
    diagnostic.endpoints_projected = start_ref != 0 && goal_ref != 0;
    if (!diagnostic.endpoints_projected)
        return diagnostic;

    diagnostic.current = inspectPathQuery(
        *navmesh.dt_query, start_ref, goal_ref, start, goal, filter,
        PATH_MAX_POLYS, QUERY_NODE_CAPACITY);

    const int expanded_corridor_capacity = std::max(PATH_MAX_POLYS, navmesh.total_polys);
    const long long desired_node_capacity = static_cast<long long>(expanded_corridor_capacity) * 8;
    const int expanded_node_capacity = static_cast<int>(std::min(
        static_cast<long long>(DIAGNOSTIC_MAX_QUERY_NODES),
        std::max(32768LL, desired_node_capacity)));
    dtNavMeshQuery* expanded_query = dtAllocNavMeshQuery();
    if (!expanded_query)
        return diagnostic;

    const dtStatus init_status = expanded_query->init(
        navmesh.dt_navmesh, expanded_node_capacity);
    diagnostic.expanded_query_initialized = !dtStatusFailed(init_status);
    if (diagnostic.expanded_query_initialized) {
        diagnostic.expanded = inspectPathQuery(
            *expanded_query, start_ref, goal_ref, start, goal, filter,
            expanded_corridor_capacity, expanded_node_capacity);
    }
    dtFreeNavMeshQuery(expanded_query);
    return diagnostic;
}

} // namespace Navigation
