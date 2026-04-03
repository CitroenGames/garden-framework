#pragma once

#include "NavMesh.hpp"
#include <string>

namespace Navigation
{

class NavMeshSerializer
{
public:
    static bool save(const NavMesh& navmesh, const std::string& filepath);
    static bool load(NavMesh& navmesh, const std::string& filepath);
};

} // namespace Navigation
