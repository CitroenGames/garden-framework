#include "NavMeshSerializer.hpp"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <fstream>
#include <cstdint>
#include <cstring>

namespace Navigation
{

static constexpr uint32_t NAVMESH_MAGIC   = 0x4E41564D; // "NAVM"
static constexpr uint32_t NAVMESH_VERSION = 2; // Version 2 = Recast/Detour format

bool NavMeshSerializer::save(const NavMesh& navmesh, const std::string& filepath)
{
    if (!navmesh.valid || !navmesh.dt_navmesh)
        return false;

    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    auto write = [&](const void* data, size_t size) {
        file.write(reinterpret_cast<const char*>(data), size);
    };

    // Header
    uint32_t magic = NAVMESH_MAGIC;
    uint32_t version = NAVMESH_VERSION;
    write(&magic, 4);
    write(&version, 4);

    // Config
    write(&navmesh.config, sizeof(NavMeshConfig));

    // Detour tile data (single tile)
    const dtNavMesh* nm = navmesh.dt_navmesh;
    const dtMeshTile* tile = nm->getTile(0);
    if (!tile || !tile->header || !tile->data)
        return false;

    int32_t data_size = tile->dataSize;
    write(&data_size, 4);
    write(tile->data, data_size);

    return file.good();
}

bool NavMeshSerializer::load(NavMesh& navmesh, const std::string& filepath)
{
    navmesh.clear();

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    auto read = [&](void* data, size_t size) {
        file.read(reinterpret_cast<char*>(data), size);
    };

    // Header
    uint32_t magic = 0, version = 0;
    read(&magic, 4);
    read(&version, 4);

    if (magic != NAVMESH_MAGIC || version != NAVMESH_VERSION)
        return false;

    // Config
    read(&navmesh.config, sizeof(NavMeshConfig));

    // Tile data
    int32_t data_size = 0;
    read(&data_size, 4);

    if (data_size <= 0 || !file.good())
        return false;

    unsigned char* navData = static_cast<unsigned char*>(dtAlloc(data_size, DT_ALLOC_PERM));
    if (!navData)
        return false;

    read(navData, data_size);
    if (!file.good())
    {
        dtFree(navData);
        return false;
    }

    // Create navmesh
    navmesh.dt_navmesh = dtAllocNavMesh();
    if (!navmesh.dt_navmesh)
    {
        dtFree(navData);
        return false;
    }

    dtStatus status = navmesh.dt_navmesh->init(navData, data_size, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        dtFree(navData);
        dtFreeNavMesh(navmesh.dt_navmesh);
        navmesh.dt_navmesh = nullptr;
        return false;
    }

    // Create query
    navmesh.dt_query = dtAllocNavMeshQuery();
    if (!navmesh.dt_query)
    {
        navmesh.clear();
        return false;
    }

    status = navmesh.dt_query->init(navmesh.dt_navmesh, 2048);
    if (dtStatusFailed(status))
    {
        navmesh.clear();
        return false;
    }

    navmesh.valid = true;

    // Count polys for stats
    navmesh.total_polys = 0;
    const dtNavMesh* cnm = navmesh.dt_navmesh;
    const dtMeshTile* tile = cnm->getTile(0);
    if (tile && tile->header)
        navmesh.total_polys = tile->header->polyCount;

    return true;
}

} // namespace Navigation
