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
        int source_triangles  = 0;
        int walkable_triangles = 0;
        float time_ms          = 0.0f;
    };

    // Build navmesh from all static collision/mesh geometry in the registry.
    static NavMesh generate(entt::registry& registry, const NavMeshConfig& config,
                            GenerationStats* stats = nullptr);

private:
    struct RawTriangle
    {
        glm::vec3 v[3];
        glm::vec3 normal;
    };

    static std::vector<RawTriangle> extractWorldTriangles(entt::registry& registry);
    static std::vector<RawTriangle> filterBySlope(const std::vector<RawTriangle>& tris,
                                                   float max_slope_degrees);
    static void buildAdjacency(NavMesh& navmesh, float merge_distance);
};

} // namespace Navigation
