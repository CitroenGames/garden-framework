#pragma once

#include "EngineExport.h"
#include "Project/PackageDependency.hpp"

#include <string>
#include <string_view>
#include <vector>

// Owns the current source-level JSON contract at package time.  It produces
// typed, direct graph edges only; path normalization and staged-file
// validation remain ProjectPackager responsibilities.
class ENGINE_API PackageLevelDependencyExtractor
{
public:
    static constexpr std::string_view SourceId = "level-v1";

    // Reads one current .level.json source artifact.  Every diagnostic names
    // `level_logical_path` and the malformed field location.
    static bool extract(const std::string& level_logical_path,
                        const std::string& staged_file_path,
                        std::vector<PackageDependencyRequest>& children,
                        std::string& error);
};
