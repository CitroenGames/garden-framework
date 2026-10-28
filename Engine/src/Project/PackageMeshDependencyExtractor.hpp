#pragma once

#include "EngineExport.h"
#include "Project/PackageDependency.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reads only the current cooked mesh contract.  Material records and LOD
// payloads are embedded in CMESH v4; external texture and collision files are
// returned as direct package edges.
class ENGINE_API PackageMeshDependencyExtractor
{
public:
    static constexpr std::string_view SourceId = "cmesh-v4";

    // `mesh_logical_path` must name the cooked .cmesh artifact.  The extractor
    // validates the complete current artifact before returning its direct
    // external children.  `format_version` and `cook_fingerprint` describe the
    // validated parent for receipt construction.
    static bool extract(const std::string& mesh_logical_path,
                        const std::string& staged_cmesh_path,
                        std::vector<PackageDependencyRequest>& children,
                        uint32_t& format_version,
                        uint64_t& cook_fingerprint,
                        std::string& error);
};
