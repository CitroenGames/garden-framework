#pragma once

#include "EngineExport.h"
#include "NavMesh.hpp"
#include <cstdint>
#include <string>

namespace Navigation
{

class ENGINE_API NavMeshSerializer
{
public:
    // The only on-disk format this serializer accepts.  Callers can expose a
    // useful diagnostic without duplicating the private file-header value.
    static constexpr uint32_t kCurrentVersion = 2;
    static bool save(const NavMesh& navmesh, const std::string& filepath);
    static bool load(NavMesh& navmesh, const std::string& filepath);
};

} // namespace Navigation
