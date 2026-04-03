#include "NavMeshSerializer.hpp"
#include <fstream>
#include <cstdint>

namespace Navigation
{

static constexpr uint32_t NAVMESH_MAGIC   = 0x4E41564D; // "NAVM"
static constexpr uint32_t NAVMESH_VERSION = 1;

bool NavMeshSerializer::save(const NavMesh& navmesh, const std::string& filepath)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    auto write = [&](const void* data, size_t size) {
        file.write(reinterpret_cast<const char*>(data), size);
    };

    // Header
    uint32_t magic = NAVMESH_MAGIC;
    uint32_t version = NAVMESH_VERSION;
    uint32_t num_triangles = static_cast<uint32_t>(navmesh.triangles.size());
    write(&magic, 4);
    write(&version, 4);
    write(&num_triangles, 4);

    // Config
    write(&navmesh.config.max_slope_angle, 4);
    write(&navmesh.config.agent_height, 4);
    write(&navmesh.config.agent_radius, 4);
    write(&navmesh.config.merge_distance, 4);

    // Triangles
    for (auto& tri : navmesh.triangles)
    {
        write(&tri.vertices[0], sizeof(glm::vec3) * 3);
        write(&tri.centroid, sizeof(glm::vec3));
        write(&tri.normal, sizeof(glm::vec3));
        write(&tri.neighbors[0], sizeof(int32_t) * 3);
        write(&tri.shared_edge[0], sizeof(int32_t) * 3);
    }

    return file.good();
}

bool NavMeshSerializer::load(NavMesh& navmesh, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    auto read = [&](void* data, size_t size) {
        file.read(reinterpret_cast<char*>(data), size);
    };

    // Header
    uint32_t magic = 0, version = 0, num_triangles = 0;
    read(&magic, 4);
    read(&version, 4);
    read(&num_triangles, 4);

    if (magic != NAVMESH_MAGIC || version != NAVMESH_VERSION)
    {
        navmesh.clear();
        return false;
    }

    // Config
    read(&navmesh.config.max_slope_angle, 4);
    read(&navmesh.config.agent_height, 4);
    read(&navmesh.config.agent_radius, 4);
    read(&navmesh.config.merge_distance, 4);

    // Triangles
    navmesh.triangles.resize(num_triangles);
    for (uint32_t i = 0; i < num_triangles; i++)
    {
        auto& tri = navmesh.triangles[i];
        read(&tri.vertices[0], sizeof(glm::vec3) * 3);
        read(&tri.centroid, sizeof(glm::vec3));
        read(&tri.normal, sizeof(glm::vec3));
        read(&tri.neighbors[0], sizeof(int32_t) * 3);
        read(&tri.shared_edge[0], sizeof(int32_t) * 3);
    }

    navmesh.valid = !navmesh.triangles.empty() && file.good();
    return navmesh.valid;
}

} // namespace Navigation
