#pragma once

#include <string>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace PathUtils
{

// Returns the directory where engines.json is stored.
// Creates it if it doesn't exist.
inline fs::path getConfigDir()
{
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata)
        return fs::path(appdata) / "Garden";
    return fs::path("C:/Garden");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home)
        return fs::path(home) / "Library" / "Application Support" / "Garden";
    return fs::path("/tmp/Garden");
#else
    // Linux: XDG_CONFIG_HOME or ~/.config
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0')
        return fs::path(xdg) / "garden";
    const char* home = std::getenv("HOME");
    if (home)
        return fs::path(home) / ".config" / "garden";
    return fs::path("/tmp/garden");
#endif
}

inline fs::path getEnginesJsonPath()
{
    return getConfigDir() / "engines.json";
}

// Returns the platform-specific editor executable name
inline std::string getEditorExeName()
{
#ifdef _WIN32
    return "Editor.exe";
#else
    return "Editor";
#endif
}

// Search for the editor executable relative to an engine root.
// Checks common build output locations.
inline fs::path findEditorPath(const fs::path& engine_root)
{
    // Check common locations in order of preference
    fs::path candidates[] = {
        engine_root / "bin" / getEditorExeName(),
        engine_root / getEditorExeName(),
        engine_root / "x64" / "Release" / getEditorExeName(),
        engine_root / "x64" / "Debug" / getEditorExeName(),
        engine_root / "build" / "Release" / getEditorExeName(),
        engine_root / "build" / "Debug" / getEditorExeName(),
    };

    for (auto& candidate : candidates)
    {
        if (fs::exists(candidate))
            return fs::canonical(candidate);
    }

    return {}; // Not found
}

} // namespace PathUtils
