#pragma once

#include "Navigation/NavMesh.hpp"
#include "Navigation/NavMeshDebugDraw.hpp"
#include <entt/entt.hpp>
#include <string>

class NavMeshPanel
{
public:
    entt::registry* registry = nullptr;

    Navigation::NavMesh navmesh;

    void draw();
    void drawDebugVisualization();

private:
    Navigation::NavMeshConfig m_config;
    Navigation::NavMeshDebugConfig m_debug_config;

    bool m_show_visualization = true;

    // Path testing
    bool m_path_test_mode = false;
    glm::vec3 m_path_start = {0.0f, 0.0f, 0.0f};
    glm::vec3 m_path_goal  = {5.0f, 0.0f, 5.0f};
    Navigation::NavPath m_test_path;

    // Serialization
    char m_filepath_buf[512] = "assets/levels/main.navmesh";

    // Stats
    int m_total_source_tris  = 0;
    int m_walkable_tris      = 0;
    float m_generation_time_ms = 0.0f;
};
