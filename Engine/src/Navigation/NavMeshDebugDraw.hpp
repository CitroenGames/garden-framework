#pragma once

#include "EngineExport.h"
#include "NavMesh.hpp"

namespace Navigation
{

struct NavMeshDebugConfig
{
    bool show_wireframe  = true;
    bool show_normals    = false;
    bool show_adjacency  = false;
    bool show_path       = false;

    glm::vec3 wireframe_color = {0.0f, 0.8f, 0.4f};
    glm::vec3 adjacency_color = {0.3f, 0.3f, 0.8f};
    glm::vec3 path_color      = {1.0f, 0.4f, 0.0f};
    glm::vec3 normal_color    = {1.0f, 1.0f, 0.0f};

    float normal_length     = 0.3f;
    float wireframe_y_offset = 0.02f;
};

class ENGINE_API NavMeshDebugDraw
{
public:
    static void draw(const NavMesh& navmesh, const NavMeshDebugConfig& config);
    static void drawPath(const NavPath& path, const glm::vec3& color, float point_size = 0.15f);
};

} // namespace Navigation
