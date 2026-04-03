#pragma once

#include <string>
#include <filesystem>

namespace EnginePaths
{
    // Returns the directory containing the running executable (e.g. .../bin/).
    std::filesystem::path getExecutableDir();

    // Resolves a path like "../assets/shaders/..." relative to the executable
    // directory instead of the current working directory.
    std::string resolveEngineAsset(const std::string& relative_path);
}
