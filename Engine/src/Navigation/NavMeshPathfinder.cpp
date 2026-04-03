#include "NavMeshPathfinder.hpp"
#include <queue>
#include <cmath>
#include <limits>

namespace Navigation
{

// ─── Point-in-triangle (XZ projection) ───────────────────────────────────────

static float sign2D(const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3)
{
    return (p1.x - p3.x) * (p2.z - p3.z) - (p2.x - p3.x) * (p1.z - p3.z);
}

static bool pointInTriangleXZ(const glm::vec3& pt, const glm::vec3& v0,
                               const glm::vec3& v1, const glm::vec3& v2)
{
    float d1 = sign2D(pt, v0, v1);
    float d2 = sign2D(pt, v1, v2);
    float d3 = sign2D(pt, v2, v0);

    bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(has_neg && has_pos);
}

// Interpolate Y on triangle at an XZ position using barycentric coords
static float interpolateY(const glm::vec3& pt, const glm::vec3& v0,
                           const glm::vec3& v1, const glm::vec3& v2)
{
    float denom = (v1.z - v2.z) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.z - v2.z);
    if (std::abs(denom) < 1e-8f) return v0.y;

    float u = ((v1.z - v2.z) * (pt.x - v2.x) + (v2.x - v1.x) * (pt.z - v2.z)) / denom;
    float v = ((v2.z - v0.z) * (pt.x - v2.x) + (v0.x - v2.x) * (pt.z - v2.z)) / denom;
    float w = 1.0f - u - v;

    return u * v0.y + v * v1.y + w * v2.y;
}

int32_t NavMeshPathfinder::findContainingTriangle(const NavMesh& navmesh, const glm::vec3& point)
{
    int32_t best = -1;
    float best_y_diff = std::numeric_limits<float>::max();

    for (int32_t i = 0; i < static_cast<int32_t>(navmesh.triangles.size()); i++)
    {
        auto& tri = navmesh.triangles[i];
        if (pointInTriangleXZ(point, tri.vertices[0], tri.vertices[1], tri.vertices[2]))
        {
            float y = interpolateY(point, tri.vertices[0], tri.vertices[1], tri.vertices[2]);
            float diff = std::abs(point.y - y);
            if (diff < best_y_diff)
            {
                best_y_diff = diff;
                best = i;
            }
        }
    }

    // If no XZ containment found, find nearest centroid
    if (best == -1)
    {
        float best_dist = std::numeric_limits<float>::max();
        for (int32_t i = 0; i < static_cast<int32_t>(navmesh.triangles.size()); i++)
        {
            float dist = glm::distance(point, navmesh.triangles[i].centroid);
            if (dist < best_dist)
            {
                best_dist = dist;
                best = i;
            }
        }
    }

    return best;
}

// ─── A* search ───────────────────────────────────────────────────────────────

std::vector<int32_t> NavMeshPathfinder::aStarSearch(const NavMesh& navmesh,
                                                     int32_t start_tri,
                                                     int32_t goal_tri)
{
    if (start_tri < 0 || goal_tri < 0)
        return {};

    if (start_tri == goal_tri)
        return {start_tri};

    int32_t n = static_cast<int32_t>(navmesh.triangles.size());

    std::vector<float> g_score(n, std::numeric_limits<float>::max());
    std::vector<int32_t> came_from(n, -1);
    std::vector<bool> closed(n, false);

    struct Node
    {
        float f;
        int32_t tri;
        bool operator>(const Node& o) const { return f > o.f; }
    };

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    g_score[start_tri] = 0.0f;
    float h = glm::distance(navmesh.triangles[start_tri].centroid,
                             navmesh.triangles[goal_tri].centroid);
    open.push({h, start_tri});

    while (!open.empty())
    {
        Node current = open.top();
        open.pop();

        if (current.tri == goal_tri)
            break;

        if (closed[current.tri])
            continue;
        closed[current.tri] = true;

        auto& tri = navmesh.triangles[current.tri];

        for (int ei = 0; ei < 3; ei++)
        {
            int32_t neighbor = tri.neighbors[ei];
            if (neighbor < 0 || closed[neighbor])
                continue;

            float edge_cost = glm::distance(tri.centroid, navmesh.triangles[neighbor].centroid);
            float tentative_g = g_score[current.tri] + edge_cost;

            if (tentative_g < g_score[neighbor])
            {
                g_score[neighbor] = tentative_g;
                came_from[neighbor] = current.tri;
                float h_n = glm::distance(navmesh.triangles[neighbor].centroid,
                                           navmesh.triangles[goal_tri].centroid);
                open.push({tentative_g + h_n, neighbor});
            }
        }
    }

    // Reconstruct path
    if (came_from[goal_tri] == -1 && start_tri != goal_tri)
        return {}; // No path

    std::vector<int32_t> corridor;
    int32_t cur = goal_tri;
    while (cur != -1)
    {
        corridor.push_back(cur);
        cur = came_from[cur];
    }

    // Reverse to get start->goal order
    std::reverse(corridor.begin(), corridor.end());
    return corridor;
}

// ─── Portal extraction ───────────────────────────────────────────────────────

std::vector<Portal> NavMeshPathfinder::getPortals(const NavMesh& navmesh,
                                                   const std::vector<int32_t>& corridor,
                                                   const glm::vec3& start,
                                                   const glm::vec3& goal)
{
    std::vector<Portal> portals;
    portals.reserve(corridor.size() + 1);

    // Start portal
    portals.push_back({start, start});

    // Edge indices: edge 0 = v0-v1, edge 1 = v1-v2, edge 2 = v2-v0
    static const int edge_verts[3][2] = {{0, 1}, {1, 2}, {2, 0}};

    for (size_t i = 0; i + 1 < corridor.size(); i++)
    {
        int32_t curr_tri = corridor[i];
        int32_t next_tri = corridor[i + 1];

        auto& tri = navmesh.triangles[curr_tri];

        // Find which edge connects to next_tri
        for (int ei = 0; ei < 3; ei++)
        {
            if (tri.neighbors[ei] == next_tri)
            {
                glm::vec3 ev0 = tri.vertices[edge_verts[ei][0]];
                glm::vec3 ev1 = tri.vertices[edge_verts[ei][1]];

                // Determine left/right based on travel direction
                glm::vec3 forward = navmesh.triangles[next_tri].centroid - tri.centroid;
                glm::vec3 edge_dir = ev1 - ev0;
                float cross_y = forward.x * edge_dir.z - forward.z * edge_dir.x;

                if (cross_y > 0.0f)
                    portals.push_back({ev0, ev1});
                else
                    portals.push_back({ev1, ev0});

                break;
            }
        }
    }

    // End portal
    portals.push_back({goal, goal});

    return portals;
}

// ─── Simple Stupid Funnel Algorithm ──────────────────────────────────────────

static float triarea2(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

static bool vequal(const glm::vec3& a, const glm::vec3& b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return (dx * dx + dz * dz) < 1e-8f;
}

std::vector<glm::vec3> NavMeshPathfinder::funnelSmooth(const std::vector<Portal>& portals,
                                                        const glm::vec3& start,
                                                        const glm::vec3& goal)
{
    std::vector<glm::vec3> path;

    if (portals.size() < 2)
    {
        path.push_back(start);
        path.push_back(goal);
        return path;
    }

    glm::vec3 portal_apex = portals[0].left;
    glm::vec3 portal_left = portals[0].left;
    glm::vec3 portal_right = portals[0].right;
    int apex_index = 0, left_index = 0, right_index = 0;

    path.push_back(portal_apex);

    int n = static_cast<int>(portals.size());

    for (int i = 1; i < n; i++)
    {
        glm::vec3 left = portals[i].left;
        glm::vec3 right = portals[i].right;

        // Update right vertex
        if (triarea2(portal_apex, portal_right, right) <= 0.0f)
        {
            if (vequal(portal_apex, portal_right) ||
                triarea2(portal_apex, portal_left, right) > 0.0f)
            {
                portal_right = right;
                right_index = i;
            }
            else
            {
                // Right over left, insert left as waypoint
                path.push_back(portal_left);
                portal_apex = portal_left;
                apex_index = left_index;
                portal_left = portal_apex;
                portal_right = portal_apex;
                left_index = apex_index;
                right_index = apex_index;
                i = apex_index;
                continue;
            }
        }

        // Update left vertex
        if (triarea2(portal_apex, portal_left, left) >= 0.0f)
        {
            if (vequal(portal_apex, portal_left) ||
                triarea2(portal_apex, portal_right, left) < 0.0f)
            {
                portal_left = left;
                left_index = i;
            }
            else
            {
                // Left over right, insert right as waypoint
                path.push_back(portal_right);
                portal_apex = portal_right;
                apex_index = right_index;
                portal_left = portal_apex;
                portal_right = portal_apex;
                left_index = apex_index;
                right_index = apex_index;
                i = apex_index;
                continue;
            }
        }
    }

    // Add goal
    if (path.empty() || glm::distance(path.back(), goal) > 1e-4f)
        path.push_back(goal);

    return path;
}

// ─── Public API ──────────────────────────────────────────────────────────────

NavPath NavMeshPathfinder::findPath(const NavMesh& navmesh,
                                     const glm::vec3& start,
                                     const glm::vec3& goal)
{
    NavPath result;

    if (!navmesh.valid || navmesh.triangles.empty())
        return result;

    int32_t start_tri = findContainingTriangle(navmesh, start);
    int32_t goal_tri = findContainingTriangle(navmesh, goal);

    if (start_tri < 0 || goal_tri < 0)
        return result;

    // Same triangle -- direct path
    if (start_tri == goal_tri)
    {
        result.waypoints.push_back(start);
        result.waypoints.push_back(goal);
        result.valid = true;
        return result;
    }

    auto corridor = aStarSearch(navmesh, start_tri, goal_tri);
    if (corridor.empty())
        return result;

    auto portals = getPortals(navmesh, corridor, start, goal);
    result.waypoints = funnelSmooth(portals, start, goal);
    result.valid = !result.waypoints.empty();

    return result;
}

} // namespace Navigation
