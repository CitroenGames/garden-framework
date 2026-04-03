#include "NavMeshDebugDraw.hpp"
#include "Debug/DebugDraw.hpp"

namespace Navigation
{

void NavMeshDebugDraw::draw(const NavMesh& navmesh, const NavMeshDebugConfig& config)
{
    if (!navmesh.valid)
        return;

    auto& dd = DebugDraw::get();
    glm::vec3 y_off(0.0f, config.wireframe_y_offset, 0.0f);

    if (config.show_wireframe)
    {
        for (auto& tri : navmesh.triangles)
        {
            dd.drawLine(tri.vertices[0] + y_off, tri.vertices[1] + y_off, config.wireframe_color);
            dd.drawLine(tri.vertices[1] + y_off, tri.vertices[2] + y_off, config.wireframe_color);
            dd.drawLine(tri.vertices[2] + y_off, tri.vertices[0] + y_off, config.wireframe_color);
        }
    }

    if (config.show_normals)
    {
        for (auto& tri : navmesh.triangles)
        {
            glm::vec3 tip = tri.centroid + tri.normal * config.normal_length;
            dd.drawLine(tri.centroid + y_off, tip + y_off, config.normal_color);
        }
    }

    if (config.show_adjacency)
    {
        for (size_t i = 0; i < navmesh.triangles.size(); i++)
        {
            auto& tri = navmesh.triangles[i];
            for (int ei = 0; ei < 3; ei++)
            {
                int32_t n = tri.neighbors[ei];
                if (n > static_cast<int32_t>(i)) // Only draw once per pair
                {
                    dd.drawLine(tri.centroid + y_off,
                                navmesh.triangles[n].centroid + y_off,
                                config.adjacency_color);
                }
            }
        }
    }
}

void NavMeshDebugDraw::drawPath(const NavPath& path, const glm::vec3& color, float point_size)
{
    if (!path.valid || path.waypoints.size() < 2)
        return;

    auto& dd = DebugDraw::get();
    glm::vec3 y_off(0.0f, 0.05f, 0.0f);

    for (size_t i = 0; i + 1 < path.waypoints.size(); i++)
    {
        dd.drawLine(path.waypoints[i] + y_off,
                     path.waypoints[i + 1] + y_off, color);
    }

    for (auto& wp : path.waypoints)
    {
        dd.drawPoint(wp + y_off, point_size, color);
    }
}

} // namespace Navigation
