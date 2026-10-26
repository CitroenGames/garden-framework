#include "Project/ProjectPackager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}
bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}
bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool testRequiredBinaries()
{
    const std::string name = "packager requires game, server and SDL3 runtime";
    auto with_server = ProjectPackager::requiredEngineBinaries(true);
    auto without_server = ProjectPackager::requiredEngineBinaries(false);
#ifdef _WIN32
    if (!contains(with_server, "Game.exe")) return fail(name, "Game.exe missing from required list");
    if (!contains(with_server, "Server.exe")) return fail(name, "Server.exe missing from required list");
    if (!contains(with_server, "SDL3.dll")) return fail(name, "SDL3.dll missing from required list");
    if (contains(with_server, "SDL2.dll")) return fail(name, "stale SDL2.dll still required");
    if (contains(without_server, "Server.exe")) return fail(name, "Server.exe required when dedicated server disabled");
#else
    if (!contains(with_server, "Game")) return fail(name, "Game missing from required list");
    if (!contains(with_server, "Server")) return fail(name, "Server missing from required list");
    if (contains(without_server, "Server")) return fail(name, "Server required when dedicated server disabled");
#endif
    if (!contains(with_server, "EngineCore.dll") && !contains(with_server, "libEngineCore.so") &&
        !contains(with_server, "libEngineCore.dylib"))
        return fail(name, "EngineCore library missing from required list");
    return pass(name);
}

bool testMissingBinariesDetected()
{
    const std::string name = "packager reports missing required binaries";
    fs::path dir = fs::temp_directory_path() / "garden_packager_tests_bin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return fail(name, "could not create temp dir");

    // Empty dir: everything required is missing.
    auto missing_empty = ProjectPackager::missingEngineBinaries(dir.string(), true);
    if (missing_empty != ProjectPackager::requiredEngineBinaries(true))
    {
        fs::remove_all(dir, ec);
        return fail(name, "empty bin dir should report every required binary missing");
    }

    // Create every required file: nothing missing.
    for (const auto& f : ProjectPackager::requiredEngineBinaries(true))
    {
        std::ofstream out(dir / f);
        out << "x";
    }
    auto missing_none = ProjectPackager::missingEngineBinaries(dir.string(), true);
    fs::remove_all(dir, ec);
    if (!missing_none.empty()) return fail(name, "populated bin dir should report no missing binaries");
    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    ok &= testRequiredBinaries();
    ok &= testMissingBinariesDetected();
    std::cout << (ok ? "PackagerTests: ALL PASS" : "PackagerTests: FAILURES") << std::endl;
    return ok ? 0 : 1;
}
