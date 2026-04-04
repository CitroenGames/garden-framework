#pragma once

#include "EngineExport.h"
#include <string>
#include <filesystem>

namespace EnginePaths
{
    // Returns the directory containing the running executable (e.g. .../bin/).
    ENGINE_API std::filesystem::path getExecutableDir();

    // Resolves a path like "../assets/shaders/..." relative to the executable
    // directory instead of the current working directory.
    ENGINE_API std::string resolveEngineAsset(const std::string& relative_path);
}
