#include "Utils/EnginePaths.hpp"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#elif defined(__APPLE__)
#   include <mach-o/dyld.h>
#else
#   include <unistd.h>
#   include <climits>
#endif

namespace fs = std::filesystem;

namespace EnginePaths
{

fs::path getExecutableDir()
{
    static fs::path cached;
    if (!cached.empty())
        return cached;

#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    cached = fs::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        cached = fs::canonical(fs::path(buf)).parent_path();
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
    {
        buf[len] = '\0';
        cached = fs::path(buf).parent_path();
    }
#endif

    if (cached.empty())
        cached = fs::current_path();
    return cached;
}

std::string resolveEngineAsset(const std::string& relative_path)
{
    return (getExecutableDir() / relative_path).string();
}

}
