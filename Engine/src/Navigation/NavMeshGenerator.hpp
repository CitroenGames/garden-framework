#pragma once

#include "EngineExport.h"
#include "NavMesh.hpp"
#include <entt/entt.hpp>

namespace Navigation
{

class ENGINE_API NavMeshGenerator
{
public:
    struct GenerationStats
    {
        int source_triangles   = 0;
        int walkable_triangles = 0;
        int total_polys        = 0;
        float time_ms          = 0.0f;
    };

    // Build navmesh from all static collision/mesh geometry in the registry
    static NavMesh generate(entt::registry& registry, const NavMeshConfig& config,
                            GenerationStats* stats = nullptr);

    // Extract debug polygon outlines from Detour navmesh (for visualization)
    static void extractDebugPolys(NavMesh& navmesh);

private:
    struct RawTriangle
    {
        glm::vec3 v[3];
        glm::vec3 normal;
    };

    static std::vector<RawTriangle> extractWorldTriangles(entt::registry& registry);
};

} // namespace Navigation
