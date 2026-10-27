#include "Project/ProjectPackager.hpp"
#include "Project/ProjectManager.hpp"
#include "LevelManager.hpp"
#include "Threading/JobSystem.hpp"
#include "Utils/EnginePaths.hpp"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
} // namespace

int main()
{
    bool ok = true;
    bool integration_skipped = false;
    ok &= testRequiredBinaries();
    ok &= testMissingBinariesDetected();
    ok &= testMissingAssetDirectoryFailsBeforeCopy();

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
        ok &= testAssetCopyFailureStopsPackaging();
        ok &= testMissingReferencedAssetFailsPackaging();
        ok &= testAssetRootRelativeReferencePassesPackaging();
        ok &= testCompiledReferenceRequiresCookedOutput();
        ok &= testFailedLevelCompileDoesNotRewriteDefaultLevel();
    }

    std::cout << (ok ? (integration_skipped ? "PackagerTests: PASS (integration tests skipped)" : "PackagerTests: ALL PASS")
                    : "PackagerTests: FAILURES") << std::endl;
    return ok ? 0 : 1;
}
