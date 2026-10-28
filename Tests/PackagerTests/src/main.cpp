#include "Assets/AssetManager.hpp"
#include "Navigation/NavMeshSerializer.hpp"
#include "Project/ProjectPackager.hpp"
#include "Project/ProjectManager.hpp"
#include "LevelManager.hpp"
#include "Threading/JobSystem.hpp"
#include "Utils/EnginePaths.hpp"
#include "../../../Templates/FPSShooter/src/shared/SelectedLevelSidecars.hpp"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

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

struct TempDirectory
{
    fs::path path;

    explicit TempDirectory(const std::string& label)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("garden_packager_" + label + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~TempDirectory()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool writeText(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << content;
    return out.good();
}

bool writeBytes(const fs::path& path, const std::vector<char>& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

bool readBytes(const fs::path& path, std::vector<char>& content)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

std::string moduleFilename()
{
#ifdef _WIN32
    return "PackagerRegression.dll";
#elif defined(__APPLE__)
    return "libPackagerRegression.dylib";
#else
    return "libPackagerRegression.so";
#endif
}

bool createProjectFixture(const fs::path& root, ProjectManager& manager,
                          const std::vector<std::string>& asset_directories,
                          const std::string& level_json,
                          bool create_asset_directories = true)
{
    std::error_code ec;
    fs::create_directories(root / "bin", ec);
    if (ec) return false;
    fs::create_directories(root / "assets" / "levels", ec);
    if (ec) return false;
    if (!writeText(root / "bin" / moduleFilename(), "module fixture")) return false;
    if (!writeText(root / "assets" / "levels" / "main.level.json", level_json)) return false;

    if (create_asset_directories)
    {
        for (const auto& asset_directory : asset_directories)
        {
            fs::create_directories(root / asset_directory, ec);
            if (ec) return false;
        }
    }

    json garden = {
        {"name", "PackagerRegression"},
        {"engine_id", "test-engine"},
        {"engine_version", "1.0"},
        {"game_module", "bin/PackagerRegression"},
        {"default_level", "assets/levels/main.level.json"},
        {"default_game_mode", "RegressionMode"},
        {"default_game_state", "RegressionState"},
        {"asset_directories", asset_directories},
        {"source_directory", "src/"},
        {"buildscript", "PackagerRegression.buildscript"},
        {"future_extension", {{"nested_value", 73}, {"names", {"alpha", "beta"}}}}
    };

    const fs::path garden_path = root / "PackagerRegression.garden";
    if (!writeText(garden_path, garden.dump(4))) return false;
    return manager.loadProject(garden_path.string());
}

PackageConfig packageConfig(const fs::path& output_root)
{
    PackageConfig config;
    config.output_directory = output_root.string();
    config.package_name = "package";
    return config;
}

#ifdef _WIN32
fs::path fpsShooterModulePath()
{
    // PackagerTests is built into <repository>/bin.  Keep the launch fixture
    // self-contained after this one source-tree lookup: the packager must
    // stage the module into the resulting package before Server.exe can load
    // it from there.
    return EnginePaths::getExecutableDir().parent_path() /
        "Templates" / "FPSShooter" / "bin" / "FPSShooter.dll";
}

bool createPackagedServerFixture(const fs::path& root, ProjectManager& manager)
{
    const fs::path module_source = fpsShooterModulePath();
    std::error_code ec;
    if (!fs::is_regular_file(module_source, ec) || ec)
        return false;

    fs::create_directories(root / "bin", ec);
    if (ec) return false;
    fs::create_directories(root / "assets" / "levels", ec);
    if (ec) return false;
    fs::copy_file(module_source, root / "bin" / "FPSShooter.dll",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    if (!writeText(root / "assets" / "levels" / "main.level.json",
                   R"({"metadata":{"level_name":"Packaged Server Fixture"},"entities":[]})"))
        return false;

    const json garden = {
        {"name", "PackagedServerFixture"},
        {"engine_id", "test-engine"},
        {"engine_version", "1.0"},
        {"game_module", "bin/FPSShooter"},
        {"default_level", "assets/levels/main.level.json"},
        {"default_game_mode", "GameMode"},
        {"default_game_state", "GameState"},
        {"asset_directories", {"assets"}},
        {"source_directory", "src/"},
        {"buildscript", "PackagedServerFixture.buildscript"},
    };
    const fs::path garden_path = root / "PackagedServerFixture.garden";
    return writeText(garden_path, garden.dump(4)) && manager.loadProject(garden_path.string());
}

std::string quoteWindowsArg(const fs::path& value)
{
    return "\"" + value.string() + "\"";
}

bool readText(const fs::path& path, std::string& out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

class ScopedEnvironmentValue
{
public:
    ScopedEnvironmentValue(const char* name, const char* value)
        : name_(name)
    {
        const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
        if (required > 0)
        {
            std::vector<char> buffer(required);
            if (GetEnvironmentVariableA(name, buffer.data(), required) > 0)
            {
                had_original_ = true;
                original_ = buffer.data();
            }
        }
        SetEnvironmentVariableA(name, value);
    }

    ~ScopedEnvironmentValue()
    {
        SetEnvironmentVariableA(name_.c_str(), had_original_ ? original_.c_str() : nullptr);
    }

private:
    std::string name_;
    std::string original_;
    bool had_original_ = false;
};

class ScopedPackagedAssetRoot
{
public:
    explicit ScopedPackagedAssetRoot(const fs::path& root)
        : assets_(Assets::AssetManager::get()), previous_root_(assets_.getAssetRoot())
    {
        assets_.setAssetRoot(root.string());
    }

    ~ScopedPackagedAssetRoot()
    {
        assets_.setAssetRoot(previous_root_);
    }

private:
    Assets::AssetManager& assets_;
    std::string previous_root_;
};

bool testPackagedDeBlock01CompetitiveServer()
{
    const std::string name = "packaged FPSShooter de_block01 competitive server preserves selected-level closure";
    const fs::path repository_root = EnginePaths::getExecutableDir().parent_path();
    const fs::path project_path = repository_root / "Templates" / "FPSShooter" / "FPSShooter.garden";
    const fs::path source_levels = repository_root / "Templates" / "FPSShooter" / "assets" / "levels";
    const fs::path source_models = repository_root / "Templates" / "FPSShooter" / "assets" / "models";
    const fs::path source_maps = repository_root / "Templates" / "FPSShooter" / "assets" / "maps";

    if (!fs::is_regular_file(project_path) || !fs::is_regular_file(fpsShooterModulePath()))
    {
        std::cout << "[SKIP] " << name << ": FPSShooter project or module is unavailable" << std::endl;
        return true;
    }

    ProjectManager project;
    if (!project.loadProject(project_path.string()))
        return fail(name, "could not load the FPSShooter project descriptor");
    if (project.getDescriptor().default_level != "assets/levels/de_block01.level.json")
        return fail(name, "the FPSShooter descriptor did not select de_block01 source level");

    TempDirectory temp("packaged_de_block01_competitive");
    PackageConfig config = packageConfig(temp.path / "out");
    config.compile_levels_to_binary = true;
    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(project, levels, config);
    if (!result.success)
        return fail(name, "could not package FPSShooter: " + result.error_message);

    const fs::path package_root = temp.path / "out" / "package";
    const fs::path packaged_descriptor = package_root / "FPSShooter.garden";
    json descriptor;
    {
        std::ifstream descriptor_file(packaged_descriptor);
        if (!descriptor_file.is_open())
            return fail(name, "packaged FPSShooter descriptor was not written");
        try { descriptor_file >> descriptor; }
        catch (...) { return fail(name, "packaged FPSShooter descriptor is not valid JSON"); }
    }
    const std::string packaged_selected_level = descriptor.value("default_level", std::string{});
    if (packaged_selected_level != "assets/levels/de_block01.level")
        return fail(name, "packaged descriptor did not select compiled de_block01.level");

    // Resolve through the package asset root, matching the server module's
    // selected-level boundary.  The test must not accidentally pass by
    // finding source-tree sidecars through its process working directory.
    ScopedPackagedAssetRoot packaged_assets(package_root / "assets");
    FpsContent::SelectedLevelSidecars packaged_sidecars;
    std::string sidecar_error;
    if (!FpsContent::resolveSelectedLevelSidecars(packaged_selected_level,
                                                  packaged_sidecars, &sidecar_error))
        return fail(name, "packaged selected-level sidecars did not resolve: " + sidecar_error);
    const std::vector<std::pair<std::string, fs::path>> expected_resolved_sidecars = {
        {packaged_sidecars.nav_path, package_root / "assets/levels/de_block01.nav"},
        {packaged_sidecars.ai_path, package_root / "assets/levels/de_block01.ai.json"},
        {packaged_sidecars.competitive_path, package_root / "assets/levels/de_block01.competitive.json"},
        {packaged_sidecars.sites_path, package_root / "assets/levels/de_block01.sites.json"},
    };
    for (const auto& [resolved, expected] : expected_resolved_sidecars) {
        if (fs::path(resolved).lexically_normal() != expected.lexically_normal())
            return fail(name, "selected-level resolver escaped package assets: " + resolved);
    }

    const std::vector<fs::path> required_artifacts = {
        "bin/FPSShooter.dll",
        "assets/levels/de_block01.level",
        "assets/levels/de_block01.competitive.json",
        "assets/levels/de_block01.sites.json",
        "assets/levels/de_block01.nav",
        "assets/levels/de_block01.ai.json",
        "assets/models/de_block01_collision.gltf",
        "assets/models/de_block01_collision.bin",
        "assets/maps/de_block01.map.json",
    };
    for (const fs::path& artifact : required_artifacts)
        if (!fs::is_regular_file(package_root / artifact))
            return fail(name, "packaged artifact is missing: " + artifact.generic_string());
    if (fs::exists(package_root / "assets" / "levels" / "de_block01.level.json"))
        return fail(name, "source de_block01 level was retained after binary compilation");

    std::vector<char> source_nav;
    std::vector<char> packaged_nav;
    if (!readBytes(source_levels / "de_block01.nav", source_nav) ||
        !readBytes(package_root / "assets" / "levels" / "de_block01.nav", packaged_nav) ||
        source_nav.empty() || source_nav != packaged_nav)
        return fail(name, "packaged de_block01 nav sidecar was not preserved byte-for-byte");

    std::ifstream packaged_nav_file(packaged_sidecars.nav_path, std::ios::binary);
    uint32_t nav_magic = 0;
    uint32_t nav_version = 0;
    packaged_nav_file.read(reinterpret_cast<char*>(&nav_magic), sizeof(nav_magic));
    packaged_nav_file.read(reinterpret_cast<char*>(&nav_version), sizeof(nav_version));
    if (!packaged_nav_file || nav_magic != 0x4E41564Du ||
        nav_version != Navigation::NavMeshSerializer::kCurrentVersion)
        return fail(name, "packaged selected nav sidecar has an unsupported serializer header");
    Navigation::NavMesh packaged_navmesh;
    if (!Navigation::NavMeshSerializer::load(packaged_navmesh, packaged_sidecars.nav_path) ||
        !packaged_navmesh.valid || packaged_navmesh.total_polys <= 0)
        return fail(name, "packaged selected nav sidecar was not loadable by the current serializer");

    const std::vector<std::pair<fs::path, fs::path>> preserved_sidecars_and_collision = {
        {source_levels / "de_block01.competitive.json", package_root / "assets/levels/de_block01.competitive.json"},
        {source_levels / "de_block01.sites.json", package_root / "assets/levels/de_block01.sites.json"},
        {source_levels / "de_block01.ai.json", package_root / "assets/levels/de_block01.ai.json"},
        {source_models / "de_block01_collision.gltf", package_root / "assets/models/de_block01_collision.gltf"},
        {source_models / "de_block01_collision.bin", package_root / "assets/models/de_block01_collision.bin"},
        {source_maps / "de_block01.map.json", package_root / "assets/maps/de_block01.map.json"},
    };
    for (const auto& [source, packaged] : preserved_sidecars_and_collision)
    {
        std::vector<char> source_bytes;
        std::vector<char> packaged_bytes;
        if (!readBytes(source, source_bytes) || !readBytes(packaged, packaged_bytes) ||
            source_bytes.empty() || source_bytes != packaged_bytes)
            return fail(name, "package did not preserve " + source.filename().string() + " byte-for-byte");
    }
    if (!fs::is_regular_file(packaged_sidecars.ai_path))
        return fail(name, "packaged selected-level resolver did not locate the bot route sidecar");

    const fs::path server_path = package_root / "bin" / "Server.exe";
    if (!fs::is_regular_file(server_path))
        return fail(name, "Server.exe was not staged into the FPSShooter package");

    const fs::path log_path = temp.path / "competitive_server.log";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log_handle = CreateFileA(log_path.string().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_handle == INVALID_HANDLE_VALUE)
        return fail(name, "could not create captured competitive server log");

    const DWORD test_port = 40000 + (GetCurrentProcessId() % 1000);
    const std::string command = quoteWindowsArg(server_path) + " --port " + std::to_string(test_port);
    std::vector<char> command_line(command.begin(), command.end());
    command_line.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_handle;
    startup.hStdError = log_handle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    {
        // Test-only process configuration. Each variable is restored before
        // this function returns so PackagerTests does not affect later tests.
        ScopedEnvironmentValue game_mode("FPS_GAME_MODE", "competitive");
        ScopedEnvironmentValue bots("FPS_BOTS", "2");
        ScopedEnvironmentValue fast_match("FPS_TEST_FAST_MATCH", "1");
        ScopedEnvironmentValue ai_debug("AI_DEBUG", "1");
        if (!CreateProcessA(server_path.string().c_str(), command_line.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW, nullptr, package_root.string().c_str(), &startup, &process))
        {
            const DWORD error = GetLastError();
            CloseHandle(log_handle);
            return fail(name, "CreateProcessA failed with error " + std::to_string(error));
        }
    }
    CloseHandle(log_handle);
    CloseHandle(process.hThread);

    const std::vector<std::string> required_log_lines = {
        "FPSShooter selected level stem=levels/de_block01",
        "Bots: navmesh status=loaded format=v" +
            std::to_string(Navigation::NavMeshSerializer::kCurrentVersion),
        "Bots: authored v2 objective data loaded",
        "Bots: spawned bot client 40000",
        "[BotDebug] bot=0",
        "path_valid=1",
        "[ACCEPT] FPS_TEST_FAST_MATCH=1",
        "Server started successfully",
    };
    bool observed_all = false;
    std::string output;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline)
    {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            break;
        if (readText(log_path, output))
        {
            observed_all = std::all_of(required_log_lines.begin(), required_log_lines.end(),
                [&](const std::string& line) { return output.find(line) != std::string::npos; });
            if (observed_all)
                break;
        }
        Sleep(25);
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 0);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait_result != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    readText(log_path, output);

    if (!observed_all)
    {
        const std::string reason = wait_result == WAIT_OBJECT_0
            ? "packaged competitive server exited before selected-level startup with code " + std::to_string(exit_code)
            : "packaged competitive server did not report selected-level navigation within 10 seconds";
        return fail(name, reason + "; log: " + output);
    }
    return pass(name);
}

bool testPackagedServerLaunch()
{
    const std::string name = "packaged server starts from the packaged directory";
    const fs::path module_source = fpsShooterModulePath();
    if (!fs::is_regular_file(module_source))
    {
        std::cout << "[SKIP] " << name << ": FPSShooter.dll is absent at "
                  << module_source.string() << std::endl;
        return true;
    }

    TempDirectory temp("packaged_server_launch");
    ProjectManager project;
    if (!createPackagedServerFixture(temp.path / "project", project))
        return fail(name, "could not create the minimal server project fixture");

    PackageConfig config = packageConfig(temp.path / "out");
    config.compile_levels_to_binary = true;
    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(project, levels, config);
    if (!result.success)
        return fail(name, "could not package the server fixture: " + result.error_message);

    const fs::path package_root = temp.path / "out" / "package";
    const fs::path server_path = package_root / "bin" / "Server.exe";
    if (!fs::is_regular_file(server_path))
        return fail(name, "Server.exe was not staged into the package");

    const fs::path log_path = temp.path / "server.log";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log_handle = CreateFileA(log_path.string().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_handle == INVALID_HANDLE_VALUE)
        return fail(name, "could not create captured server log");

    // Do not share the normal developer-server port. A PID-derived high port
    // makes parallel test processes unlikely to contend while retaining an
    // explicit port for this bounded launch contract.
    const DWORD test_port = 39000 + (GetCurrentProcessId() % 1000);
    const std::string command = quoteWindowsArg(server_path) + " --port " + std::to_string(test_port);
    std::vector<char> command_line(command.begin(), command.end());
    command_line.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_handle;
    startup.hStdError = log_handle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(server_path.string().c_str(), command_line.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, package_root.string().c_str(), &startup, &process))
    {
        const DWORD error = GetLastError();
        CloseHandle(log_handle);
        return fail(name, "CreateProcessA failed with error " + std::to_string(error));
    }
    CloseHandle(log_handle);
    CloseHandle(process.hThread);

    bool started = false;
    std::string output;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline)
    {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            break;
        if (readText(log_path, output) && output.find("Server started successfully") != std::string::npos)
        {
            started = true;
            break;
        }
        Sleep(25);
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 0);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait_result != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    readText(log_path, output);

    if (!started)
    {
        const std::string reason = wait_result == WAIT_OBJECT_0
            ? "packaged server exited before startup with code " + std::to_string(exit_code)
            : "packaged server did not report startup within 5 seconds";
        return fail(name, reason + "; log: " + output);
    }
    return pass(name);
}
#endif

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

bool testMissingAssetDirectoryFailsBeforeCopy()
{
    const std::string name = "packager fails when a declared asset directory is missing";
    TempDirectory temp("missing_asset_dir");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets_missing"},
                              R"({"metadata":{"level_name":"Missing Assets"},"entities":[]})", false))
        return fail(name, "could not create project fixture");

    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(
        project, levels, packageConfig(temp.path / "out"));
    if (result.success) return fail(name, "package succeeded with a missing declared asset directory");
    if (result.error_message.find("Asset directory not found:") == std::string::npos)
        return fail(name, "failure did not name the missing asset directory: " + result.error_message);
    return pass(name);
}

bool testPackagePathsCannotEscapeRoots()
{
    const std::string name = "packager rejects package metadata paths outside its roots";
    TempDirectory temp("path_roots");
    LevelManager levels;

    ProjectManager escaped_assets;
    if (!createProjectFixture(temp.path / "asset_project", escaped_assets, {"../outside_assets"},
                              R"({"metadata":{"level_name":"Escaped Assets"},"entities":[]})", false))
        return fail(name, "could not create asset-path fixture");
    PackageResult result = ProjectPackager::packageProject(
        escaped_assets, levels, packageConfig(temp.path / "asset_out"));
    if (result.success || result.error_message.find("Asset directory must be project-relative") == std::string::npos)
        return fail(name, "parent-traversal asset directory was accepted: " + result.error_message);
    if (fs::exists(temp.path / "asset_out" / "package"))
        return fail(name, "asset traversal created a package output before failing");

    ProjectManager escaped_level;
    if (!createProjectFixture(temp.path / "level_project", escaped_level, {"assets"},
                              R"({"metadata":{"level_name":"Escaped Level"},"entities":[]})"))
        return fail(name, "could not create level-path fixture");
    escaped_level.getDescriptor().default_level = "../outside.level";
    result = ProjectPackager::packageProject(escaped_level, levels, packageConfig(temp.path / "level_out"));
    if (result.success || result.error_message.find("Default level must be project-relative") == std::string::npos)
        return fail(name, "parent-traversal default level was accepted: " + result.error_message);
    if (fs::exists(temp.path / "level_out" / "package"))
        return fail(name, "default-level traversal created a package output before failing");

    ProjectManager package_name;
    if (!createProjectFixture(temp.path / "name_project", package_name, {"assets"},
                              R"({"metadata":{"level_name":"Package Name"},"entities":[]})"))
        return fail(name, "could not create package-name fixture");
    PackageConfig invalid_name = packageConfig(temp.path / "name_out");
    invalid_name.package_name = "../outside_package";
    result = ProjectPackager::packageProject(package_name, levels, invalid_name);
    if (result.success || result.error_message.find("Package name must be a single relative") == std::string::npos)
        return fail(name, "parent-traversal package name was accepted: " + result.error_message);
    if (fs::exists(temp.path / "outside_package"))
        return fail(name, "package-name traversal created output outside its requested root");

    ProjectManager nested_output;
    if (!createProjectFixture(temp.path / "nested_project", nested_output, {"assets"},
                              R"({"metadata":{"level_name":"Nested Output"},"entities":[]})"))
        return fail(name, "could not create nested-output fixture");
    PackageConfig equal_to_asset = packageConfig(temp.path / "nested_project");
    equal_to_asset.package_name = "assets";
    result = ProjectPackager::packageProject(nested_output, levels, equal_to_asset);
    if (result.success || result.error_message.find("Output directory cannot be inside project asset directory") == std::string::npos)
        return fail(name, "output equal to an asset directory was accepted: " + result.error_message);
    return pass(name);
}

bool testManifestUnknownFieldsRoundTrip()
{
    const std::string name = "packager preserves unknown .garden fields";
    TempDirectory temp("manifest_roundtrip");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets"},
                              R"({"metadata":{"level_name":"Manifest Round Trip"},"entities":[]})"))
        return fail(name, "could not create project fixture");

    PackageConfig config = packageConfig(temp.path / "out");
    config.compile_levels_to_binary = true;
    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(project, levels, config);
    if (!result.success) return fail(name, "package failed: " + result.error_message);

    const fs::path package_bin = temp.path / "out" / "package" / "bin";
    for (const auto& binary : ProjectPackager::requiredEngineBinaries(true))
    {
        if (!fs::exists(package_bin / binary))
            return fail(name, "required server-enabled package binary was not staged: " + binary);
    }

    std::ifstream in(temp.path / "out" / "package" / "PackagerRegression.garden");
    json packaged;
    if (!in.is_open()) return fail(name, "packaged .garden file was not written");
    try
    {
        in >> packaged;
    }
    catch (...)
    {
        return fail(name, "packaged .garden file is not valid JSON");
    }

    if (!packaged.contains("future_extension") ||
        packaged["future_extension"].value("nested_value", 0) != 73 ||
        packaged["future_extension"].value("names", std::vector<std::string>{}) != std::vector<std::string>{"alpha", "beta"})
        return fail(name, "unknown nested manifest fields were dropped or changed");
    if (packaged.value("default_game_mode", std::string{}) != "RegressionMode" ||
        packaged.value("default_game_state", std::string{}) != "RegressionState")
        return fail(name, "known gameplay defaults were not preserved");
    if (packaged.value("default_level", std::string{}) != "assets/levels/main.level")
        return fail(name, "successfully compiled default level did not use its binary artifact");
    if (!fs::exists(temp.path / "out" / "package" / "assets" / "levels" / "main.level"))
        return fail(name, "compiled default level artifact was not staged");
    return pass(name);
}

bool testNavSidecarSurvivesRawAndCookedPackaging()
{
    const std::string name = "packager preserves binary nav sidecars in raw and cooked packages";
    TempDirectory temp("nav_sidecar");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets"},
                              R"({"metadata":{"level_name":"Nav Sidecar"},"entities":[]})"))
        return fail(name, "could not create project fixture");

    const std::vector<char> nav_bytes = {
        'N', 'A', 'V', '0', '\0', '\x7f', '\x01', '\x02', '\xff', '\n', 's', 'i', 'd', 'e', 'c', 'a', 'r'
    };
    const fs::path source_nav = temp.path / "project" / "assets" / "levels" / "main.nav";
    if (!writeBytes(source_nav, nav_bytes))
        return fail(name, "could not write binary nav fixture");

    auto verify_sidecar = [&](const fs::path& package_root, const std::string& path_name) {
        std::vector<char> packaged_bytes;
        const fs::path packaged_nav = package_root / "assets" / "levels" / "main.nav";
        if (!readBytes(packaged_nav, packaged_bytes))
            return fail(name, path_name + " package did not contain the nav sidecar");
        if (packaged_bytes != nav_bytes)
            return fail(name, path_name + " package changed the nav sidecar bytes");
        return true;
    };

    LevelManager levels;
    PackageConfig raw_config = packageConfig(temp.path / "raw_out");
    const PackageResult raw_result = ProjectPackager::packageProject(project, levels, raw_config);
    if (!raw_result.success)
        return fail(name, "raw package failed: " + raw_result.error_message);
    if (!verify_sidecar(temp.path / "raw_out" / "package", "raw"))
        return false;

    auto& job_system = Threading::JobSystem::get();
    const bool initialized_here = !job_system.isInitialized();
    if (initialized_here && !job_system.initialize(1))
        return fail(name, "could not initialize the asset compiler job system");

    PackageConfig cooked_config = packageConfig(temp.path / "cooked_out");
    cooked_config.compile_assets = true;
    cooked_config.compile_levels_to_binary = true;
    const PackageResult cooked_result = ProjectPackager::packageProject(project, levels, cooked_config);
    if (initialized_here)
        job_system.shutdown();

    if (!cooked_result.success)
        return fail(name, "cooked package failed: " + cooked_result.error_message);
    const fs::path cooked_root = temp.path / "cooked_out" / "package";
    if (!verify_sidecar(cooked_root, "cooked"))
        return false;
    if (!fs::is_regular_file(cooked_root / "assets" / "levels" / "main.level"))
        return fail(name, "cooked package did not compile the level alongside its nav sidecar");
    return pass(name);
}

bool testAssetCopyFailureStopsPackaging()
{
    const std::string name = "packager fails on project asset copy errors";
    TempDirectory temp("asset_copy_failure");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"content"},
                              R"({"metadata":{"level_name":"Copy Failure"},"entities":[]})"))
        return fail(name, "could not create project fixture");

    const fs::path output_root = temp.path / "out" / "package";
    std::error_code ec;
    fs::create_directories(output_root, ec);
    if (ec) return fail(name, "could not create package obstruction");
    if (!writeText(output_root / "content", "file blocks destination directory"))
        return fail(name, "could not create destination obstruction");

    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(
        project, levels, packageConfig(temp.path / "out"));
    if (result.success) return fail(name, "package succeeded after failing to copy project assets");
    if (result.error_message.find("Failed to create directory") == std::string::npos)
        return fail(name, "failure did not identify the copy error: " + result.error_message);
    return pass(name);
}

bool testMissingReferencedAssetFailsPackaging()
{
    const std::string name = "packager fails when a level references a missing asset";
    TempDirectory temp("missing_reference");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets"},
                              R"({"metadata":{"level_name":"Missing Reference"},"entities":[{"mesh":{"path":"assets/models/missing.gltf"}}]})"))
        return fail(name, "could not create project fixture");

    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(
        project, levels, packageConfig(temp.path / "out"));
    if (result.success) return fail(name, "package succeeded with a missing level asset");
    if (result.error_message.find("Referenced asset missing from package: assets/models/missing.gltf") == std::string::npos)
        return fail(name, "failure did not name the missing referenced asset: " + result.error_message);
    return pass(name);
}

bool testAssetRootRelativeReferencePassesPackaging()
{
    const std::string name = "packager resolves level assets relative to the configured asset root";
    TempDirectory temp("asset_root_reference");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets/"},
                              R"({"metadata":{"level_name":"Asset Root Reference"},"entities":[{"mesh":{"path":"assets/models/kept.gltf","textures":["textures/foo.bmp"]}}]})"))
        return fail(name, "could not create project fixture");

    if (!writeText(temp.path / "project" / "assets" / "textures" / "foo.bmp", "texture fixture"))
        return fail(name, "could not create the referenced texture under the configured asset root");
    if (!writeText(temp.path / "project" / "assets" / "models" / "kept.gltf", "model fixture"))
        return fail(name, "could not create the explicitly project-relative model");

    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(
        project, levels, packageConfig(temp.path / "out"));
    if (!result.success) return fail(name, "package failed: " + result.error_message);
    if (!fs::exists(temp.path / "out" / "package" / "assets" / "textures" / "foo.bmp"))
        return fail(name, "referenced texture was not copied under the configured asset root");
    if (!fs::exists(temp.path / "out" / "package" / "assets" / "models" / "kept.gltf"))
        return fail(name, "explicit project-relative model was not preserved without a duplicate asset prefix");
    return pass(name);
}

bool testCompiledReferenceRequiresCookedOutput()
{
    const std::string name = "packager requires cooked output for compiled asset references";
    TempDirectory temp("stale_raw_reference");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets"},
                              R"({"metadata":{"level_name":"Stale Raw Reference"},"entities":[{"mesh":{"path":"assets/models/stale.gltf"}}]})"))
        return fail(name, "could not create project fixture");

    // Simulate an output directory reused from an older raw-asset package.
    // AssetCompiler will not see this file in the source tree and therefore
    // will not produce its required .cmesh counterpart.
    const fs::path stale_raw = temp.path / "out" / "package" / "assets" / "models" / "stale.gltf";
    if (!writeText(stale_raw, "stale raw model"))
        return fail(name, "could not create stale package artifact");

    auto& job_system = Threading::JobSystem::get();
    const bool initialized_here = !job_system.isInitialized();
    if (initialized_here && !job_system.initialize(1))
        return fail(name, "could not initialize the asset compiler job system");

    PackageConfig config = packageConfig(temp.path / "out");
    config.compile_assets = true;
    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(project, levels, config);
    if (initialized_here)
        job_system.shutdown();

    if (result.success)
        return fail(name, "package accepted a stale raw model without the required .cmesh artifact");
    if (result.error_message.find("Referenced asset missing from package: assets/models/stale.gltf") == std::string::npos)
        return fail(name, "failure did not identify the missing cooked reference: " + result.error_message);
    if (!fs::exists(stale_raw))
        return fail(name, "test did not retain the stale raw artifact that should not satisfy validation");
    if (fs::exists(temp.path / "out" / "package" / "assets" / "models" / "stale.cmesh"))
        return fail(name, "unexpected cooked artifact was generated from a missing source model");
    return pass(name);
}

bool testFailedLevelCompileDoesNotRewriteDefaultLevel()
{
    const std::string name = "packager fails level compilation without rewriting the default level";
    TempDirectory temp("level_compile_failure");
    ProjectManager project;
    if (!createProjectFixture(temp.path / "project", project, {"assets"},
                              R"({"metadata":{"level_name":"Compile Output Block"},"entities":[]})"))
        return fail(name, "could not create project fixture");

    PackageConfig config = packageConfig(temp.path / "out");
    config.compile_levels_to_binary = true;
    const fs::path package_root = temp.path / "out" / "package";
    std::error_code ec;
    fs::create_directories(package_root / "assets" / "levels" / "main.level", ec);
    if (ec) return fail(name, "could not block binary level output: " + ec.message());

    LevelManager levels;
    const PackageResult result = ProjectPackager::packageProject(project, levels, config);
    if (result.success) return fail(name, "package succeeded after level compilation failed");
    if (result.error_message.find("Failed to compile level: main.level.json") == std::string::npos)
        return fail(name, "failure did not name the uncompiled level: " + result.error_message);

    if (fs::exists(package_root / "PackagerRegression.garden"))
        return fail(name, "manifest was written despite compilation failure");
    if (!fs::exists(package_root / "assets" / "levels" / "main.level.json"))
        return fail(name, "source default level was removed after compilation failed");
    return pass(name);
}

bool packageIntegrationBinariesAvailable(std::vector<std::string>& missing)
{
    missing = ProjectPackager::missingEngineBinaries(EnginePaths::getExecutableDir().string(), true);
    return missing.empty();
}

bool testSelectedLevelArtifactPathNormalization()
{
    const std::string name = "Selected level artifact path normalization";
    const auto source = ProjectManager::normalizeSelectedLevelArtifactPath(
        "assets/levels/../levels/de_block01.level.json");
    if (!source || *source != "assets/levels/de_block01.level.json")
        return fail(name, "source artifact was not normalized");

    const auto packaged = ProjectManager::normalizeSelectedLevelArtifactPath(
        "assets/levels/de_block01.level");
    if (!packaged || *packaged != "assets/levels/de_block01.level")
        return fail(name, "packaged artifact was not accepted");

    for (const std::string& malformed : {"../escape.level.json", "assets/levels/escape.json", "", "C:/outside.level"})
        if (ProjectManager::normalizeSelectedLevelArtifactPath(malformed))
            return fail(name, "invalid artifact was accepted: " + malformed);

    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    bool integration_skipped = false;
    ok &= testRequiredBinaries();
    ok &= testMissingBinariesDetected();
    ok &= testMissingAssetDirectoryFailsBeforeCopy();
    ok &= testPackagePathsCannotEscapeRoots();
    ok &= testSelectedLevelArtifactPathNormalization();

    std::vector<std::string> missing_runtime_files;
    if (!packageIntegrationBinariesAvailable(missing_runtime_files))
    {
        integration_skipped = true;
        std::cout << "[SKIP] package integration regressions: required runtime binaries are absent from the test executable directory:";
        for (const auto& file : missing_runtime_files) std::cout << " " << file;
        std::cout << std::endl;
    }
    else
    {
        ok &= testManifestUnknownFieldsRoundTrip();
        ok &= testNavSidecarSurvivesRawAndCookedPackaging();
        ok &= testAssetCopyFailureStopsPackaging();
        ok &= testMissingReferencedAssetFailsPackaging();
        ok &= testAssetRootRelativeReferencePassesPackaging();
        ok &= testCompiledReferenceRequiresCookedOutput();
        ok &= testFailedLevelCompileDoesNotRewriteDefaultLevel();
#ifdef _WIN32
        ok &= testPackagedServerLaunch();
        ok &= testPackagedDeBlock01CompetitiveServer();
#endif
    }

    std::cout << (ok ? (integration_skipped ? "PackagerTests: PASS (integration tests skipped)" : "PackagerTests: ALL PASS")
                    : "PackagerTests: FAILURES") << std::endl;
    return ok ? 0 : 1;
}
