#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Navigation
{

struct NavTriangle
{
    glm::vec3 vertices[3];
    glm::vec3 centroid;
    glm::vec3 normal;

    // Adjacency: index into NavMesh::triangles for each edge (0-1, 1-2, 2-0)
    // -1 means no neighbor on that edge
    int32_t neighbors[3] = {-1, -1, -1};
    int32_t shared_edge[3] = {-1, -1, -1};
};

struct Portal
{
    glm::vec3 left;
    glm::vec3 right;
};

struct NavPath
{
    std::vector<glm::vec3> waypoints;
    bool valid = false;
};

struct NavMeshConfig
{
    float max_slope_angle = 45.0f;   // Degrees
    float agent_height    = 1.8f;
    float agent_radius    = 0.3f;
    float merge_distance  = 0.001f;  // Vertex snap tolerance for edge matching
};

struct NavMesh
{
    std::vector<NavTriangle> triangles;
    NavMeshConfig config;
    bool valid = false;

    void clear()
    {
        triangles.clear();
        valid = false;
    }
};

} // namespace Navigation
