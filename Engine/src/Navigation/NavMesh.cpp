#include "NavMesh.hpp"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

namespace Navigation
{

NavMesh::NavMesh() = default;

NavMesh::~NavMesh()
{
    clear();
}

NavMesh::NavMesh(NavMesh&& other) noexcept
    : dt_navmesh(other.dt_navmesh)
    , dt_query(other.dt_query)
    , config(other.config)
    , valid(other.valid)
    , debug_polys(std::move(other.debug_polys))
    , total_polys(other.total_polys)
{
    other.dt_navmesh = nullptr;
    other.dt_query = nullptr;
    other.valid = false;
}

NavMesh& NavMesh::operator=(NavMesh&& other) noexcept
{
    if (this != &other)
    {
        clear();
        dt_navmesh = other.dt_navmesh;
        dt_query = other.dt_query;
        config = other.config;
        valid = other.valid;
        debug_polys = std::move(other.debug_polys);
        total_polys = other.total_polys;

        other.dt_navmesh = nullptr;
        other.dt_query = nullptr;
        other.valid = false;
    }
    return *this;
}

void NavMesh::clear()
{
    if (dt_query)
    {
        dtFreeNavMeshQuery(dt_query);
        dt_query = nullptr;
    }
    if (dt_navmesh)
    {
        dtFreeNavMesh(dt_navmesh);
        dt_navmesh = nullptr;
    }
    debug_polys.clear();
    valid = false;
    total_polys = 0;
}

} // namespace Navigation
