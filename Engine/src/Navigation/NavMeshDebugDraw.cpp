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
        for (auto& poly : navmesh.debug_polys)
        {
            size_t n = poly.vertices.size();
            for (size_t i = 0; i < n; i++)
            {
                size_t j = (i + 1) % n;
                dd.drawLine(poly.vertices[i] + y_off,
                             poly.vertices[j] + y_off,
                             config.wireframe_color);
            }
        }
    }

    if (config.show_normals)
    {
        for (auto& poly : navmesh.debug_polys)
        {
            // Approximate normal from first 3 vertices
            if (poly.vertices.size() >= 3)
            {
                glm::vec3 edge1 = poly.vertices[1] - poly.vertices[0];
                glm::vec3 edge2 = poly.vertices[2] - poly.vertices[0];
                glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));

                glm::vec3 tip = poly.centroid + normal * config.normal_length;
                dd.drawLine(poly.centroid + y_off, tip + y_off, config.normal_color);
            }
        }
    }

    if (config.show_adjacency)
    {
        // Draw centroid-to-centroid connections between adjacent polygons
        for (size_t i = 0; i < navmesh.debug_polys.size(); i++)
        {
            for (size_t j = i + 1; j < navmesh.debug_polys.size(); j++)
            {
                // Check if polygons share an edge (simple distance heuristic)
                float dist = glm::distance(navmesh.debug_polys[i].centroid,
                                            navmesh.debug_polys[j].centroid);
                // Only draw for nearby polygons (likely adjacent)
                if (dist < navmesh.config.cell_size * navmesh.config.max_edge_len * 1.5f)
                {
                    dd.drawLine(navmesh.debug_polys[i].centroid + y_off,
                                 navmesh.debug_polys[j].centroid + y_off,
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
