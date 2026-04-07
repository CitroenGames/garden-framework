#include "ProjectPackager.hpp"
#include "ProjectManager.hpp"
#include "LevelManager.hpp"
#include "Assets/AssetManager.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string shaderSubdir(RenderAPIType api)
{
    switch (api)
    {
    case RenderAPIType::D3D11:  return "d3d11";
    case RenderAPIType::D3D12:  return "d3d12";
    case RenderAPIType::Vulkan: return "vulkan";
    case RenderAPIType::Metal:  return "metal";
    default:                    return "d3d11";
    }
}

static int copyDirectoryRecursive(const fs::path& src, const fs::path& dst,
                                  std::vector<std::string>* warnings_out = nullptr)
{
    if (!fs::exists(src) || !fs::is_directory(src))
        return 0;

    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec)
    {
        std::string msg = "Failed to create directory '" + dst.string() + "': " + ec.message();
        LOG_ENGINE_ERROR("[Packager] {}", msg);
        if (warnings_out) warnings_out->push_back(msg);
        return 0;
    }

    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec) break;

        fs::path relative = fs::relative(entry.path(), src, ec);
        if (ec) continue;

        fs::path dest_path = dst / relative;

        if (entry.is_directory())
        {
            fs::create_directories(dest_path, ec);
        }
        else if (entry.is_regular_file())
        {
            fs::create_directories(dest_path.parent_path(), ec);
            fs::copy_file(entry.path(), dest_path, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                std::string msg = "Failed to copy '" + entry.path().string() + "': " + ec.message();
                LOG_ENGINE_WARN("[Packager] {}", msg);
                if (warnings_out) warnings_out->push_back(msg);
            }
            else
                ++count;
        }
    }
    return count;
}

static bool copySingleFile(const fs::path& src, const fs::path& dst, const char* label,
                           std::vector<std::string>* warnings_out = nullptr)
{
    if (!fs::exists(src))
    {
        std::string msg = std::string(label) + " not found: " + src.string();
        LOG_ENGINE_ERROR("[Packager] {}", msg);
        if (warnings_out) warnings_out->push_back(msg);
        return false;
    }

    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        std::string msg = std::string("Failed to copy ") + label + ": " + ec.message();
        LOG_ENGINE_ERROR("[Packager] {}", msg);
        if (warnings_out) warnings_out->push_back(msg);
        return false;
    }
    return true;
}

PackageResult ProjectPackager::packageProject(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config)
{
    // Synchronous overload — no progress reporting
    return packageProjectInternal(project_manager, level_manager, config, nullptr);
}

void ProjectPackager::packageProjectAsync(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config,
    std::shared_ptr<PackageProgress> progress)
{
    std::thread([&project_manager, &level_manager, config, progress]() {
        PackageResult result = packageProjectInternal(
            project_manager, level_manager, config, progress);
        progress->result = std::move(result);
        progress->finished.store(true, std::memory_order_release);
    }).detach();
}

PackageResult ProjectPackager::packageProjectInternal(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config,
    std::shared_ptr<PackageProgress> progress)
{
    PackageResult result;

    auto setPhase = [&](PackageProgress::Phase phase) {
        if (progress)
            progress->current_phase.store(phase, std::memory_order_release);
    };

    // --- Validate ---
    if (!project_manager.isLoaded())
    {
        result.error_message = "No project loaded.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    if (config.output_directory.empty() || config.package_name.empty())
    {
        result.error_message = "Output directory and package name are required.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    const auto& desc = project_manager.getDescriptor();
    const fs::path project_root = project_manager.getProjectRoot();
    const fs::path output_root = fs::path(config.output_directory) / config.package_name;

    // Check game module DLL exists
    std::string module_path = project_manager.getAbsoluteModulePath();
    if (!module_path.empty() && !fs::exists(module_path))
    {
        result.error_message = "Game module DLL not found: " + module_path +
            "\nBuild the game module before packaging.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Prevent output inside project asset directories
    for (const auto& asset_dir : desc.asset_directories)
    {
        fs::path abs_asset = fs::absolute(project_root / asset_dir);
        fs::path abs_output = fs::absolute(output_root);
        std::error_code ec;
        fs::path rel = fs::relative(abs_output, abs_asset, ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.')
        {
            result.error_message = "Output directory cannot be inside project asset directory: " + abs_asset.string();
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    LOG_ENGINE_INFO("[Packager] Packaging '{}' to '{}'...", desc.name, output_root.string());

    // --- Create output structure ---
    std::error_code ec;
    fs::create_directories(output_root / "bin", ec);
    fs::create_directories(output_root / "assets", ec);
    if (ec)
    {
        result.error_message = "Failed to create output directories: " + ec.message();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // --- Copy engine binaries ---
    setPhase(PackageProgress::Phase::CopyingBinaries);

    fs::path engine_bin = EnginePaths::getExecutableDir();

#ifdef _WIN32
    const char* engine_files[] = { "Game.exe", "EngineCore.dll", "EngineGraphics.dll", "SDL2.dll" };
#elif defined(__APPLE__)
    const char* engine_files[] = { "Game", "libEngineCore.dylib", "libEngineGraphics.dylib" };
#else
    const char* engine_files[] = { "Game", "libEngineCore.so", "libEngineGraphics.so" };
#endif

    for (const char* file : engine_files)
    {
        if (copySingleFile(engine_bin / file, output_root / "bin" / file, file, &result.warnings))
            result.files_copied++;
        else
        {
            std::string msg = std::string("Missing engine binary: ") + file;
            LOG_ENGINE_WARN("[Packager] {}", msg);
            result.warnings.push_back(msg);
        }
    }

    // --- Copy game module DLL ---
    if (!module_path.empty() && fs::exists(module_path))
    {
        fs::path module_filename = fs::path(module_path).filename();
        if (copySingleFile(module_path, output_root / "bin" / module_filename, "Game module", &result.warnings))
            result.files_copied++;
    }

    // --- Copy engine assets ---
    fs::path engine_assets = fs::path(engine_bin) / ".." / "assets";
    engine_assets = fs::weakly_canonical(engine_assets);

    // Shaders – copy all platform variants available on this OS
#ifdef _WIN32
    const std::vector<std::string> shader_dirs = { "d3d11", "d3d12", "vulkan" };
#elif __APPLE__
    const std::vector<std::string> shader_dirs = { "metal" };
#else
    const std::vector<std::string> shader_dirs = { "vulkan" };
#endif
    for (const auto& sd : shader_dirs)
    {
        fs::path shader_src = engine_assets / "shaders" / "compiled" / sd;
        fs::path shader_dst = output_root / "assets" / "shaders" / "compiled" / sd;
        if (fs::exists(shader_src))
        {
            LOG_ENGINE_INFO("[Packager] Copying {} shaders...", sd);
            result.files_copied += copyDirectoryRecursive(shader_src, shader_dst, &result.warnings);
        }
    }

    // Fonts
    fs::path fonts_src = engine_assets / "fonts";
    if (fs::exists(fonts_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying fonts...");
        result.files_copied += copyDirectoryRecursive(fonts_src, output_root / "assets" / "fonts", &result.warnings);
    }

    // UI
    fs::path ui_src = engine_assets / "ui";
    if (fs::exists(ui_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying UI assets...");
        result.files_copied += copyDirectoryRecursive(ui_src, output_root / "assets" / "ui", &result.warnings);
    }

    // --- Copy/compile project assets ---
    setPhase(PackageProgress::Phase::CompilingAssets);

    for (const auto& asset_dir : desc.asset_directories)
    {
        fs::path src = project_root / asset_dir;
        if (!fs::exists(src))
        {
            std::string msg = "Asset directory not found: " + src.string();
            LOG_ENGINE_WARN("[Packager] {}", msg);
            result.warnings.push_back(msg);
            continue;
        }

        fs::path dst = output_root / asset_dir;

        if (config.compile_assets)
        {
            LOG_ENGINE_INFO("[Packager] Compiling project assets from '{}'...", asset_dir);

            // Build progress callback that forwards to PackageProgress
            Assets::CompileProgressCallback compile_cb = nullptr;
            if (progress) {
                compile_cb = [&progress](const Assets::CompileProgress& cp) {
                    progress->total_assets.store(cp.total_assets, std::memory_order_relaxed);
                    progress->completed_assets.store(cp.completed_assets, std::memory_order_relaxed);
                    progress->skipped_assets.store(cp.skipped_assets, std::memory_order_relaxed);
                    progress->failed_assets.store(cp.failed_assets, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(progress->current_asset_mutex);
                        progress->current_asset = cp.current_asset;
                    }
                };
            }

            Assets::CompileProgress compile_result = Assets::AssetCompiler::compileAll(
                src.string(), dst.string(), config.compile_config, compile_cb);

            result.models_compiled  += compile_result.models_compiled;
            result.textures_compiled += compile_result.textures_compiled;
            result.assets_skipped   += compile_result.skipped_assets;
            result.files_copied     += compile_result.completed_assets;

            for (const auto& err : compile_result.errors)
                result.warnings.push_back(err);
            for (const auto& warn : compile_result.warnings)
                result.warnings.push_back(warn);

            if (compile_result.failed_assets > 0)
                LOG_ENGINE_WARN("[Packager] {} asset(s) failed to compile", compile_result.failed_assets);
            else
                LOG_ENGINE_INFO("[Packager] Asset compilation complete: {} models, {} textures, {} skipped",
                                compile_result.models_compiled, compile_result.textures_compiled, compile_result.skipped_assets);
        }
        else
        {
            LOG_ENGINE_INFO("[Packager] Copying project assets from '{}'...", asset_dir);
            result.files_copied += copyDirectoryRecursive(src, dst, &result.warnings);
        }
    }

    // --- Validate referenced assets ---
    setPhase(PackageProgress::Phase::ValidatingAssets);
    {
        // Scan all level files in the output to find referenced asset paths
        fs::path output_levels = output_root / "assets" / "levels";
        if (fs::exists(output_levels))
        {
            auto referenced = Assets::AssetManager::collectLevelAssets(output_levels.string());
            int missing = 0;
            for (const auto& asset_path : referenced)
            {
                fs::path full = output_root / asset_path;
                if (!fs::exists(full))
                {
                    std::string msg = "Referenced asset missing from package: " + asset_path;
                    LOG_ENGINE_WARN("[Packager] {}", msg);
                    result.warnings.push_back(msg);
                    ++missing;
                }
            }
            if (missing > 0)
                LOG_ENGINE_WARN("[Packager] {} referenced asset(s) missing from package", missing);
            else if (!referenced.empty())
                LOG_ENGINE_INFO("[Packager] All {} referenced assets validated", referenced.size());
        }
    }

    // --- Optionally compile levels to binary ---
    std::string default_level = desc.default_level;

    if (config.compile_levels_to_binary)
    {
        setPhase(PackageProgress::Phase::CompilingLevels);
        LOG_ENGINE_INFO("[Packager] Compiling levels to binary...");

        // Find all .level.json files in the output
        for (const auto& entry : fs::recursive_directory_iterator(output_root / "assets", ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            if (filename.size() > 11 && filename.substr(filename.size() - 11) == ".level.json")
            {
                std::string json_path = entry.path().string();
                std::string binary_path = json_path.substr(0, json_path.size() - 11) + ".level";

                if (level_manager.compileLevel(json_path, binary_path))
                {
                    result.levels_compiled++;
                    // Remove the JSON original
                    fs::remove(entry.path(), ec);
                    LOG_ENGINE_INFO("[Packager] Compiled: {}", filename);
                }
                else
                {
                    std::string msg = "Failed to compile level: " + filename;
                    LOG_ENGINE_WARN("[Packager] {}", msg);
                    result.warnings.push_back(msg);
                }
            }
        }

        // Update default_level extension
        if (default_level.size() > 11 &&
            default_level.substr(default_level.size() - 11) == ".level.json")
        {
            default_level = default_level.substr(0, default_level.size() - 11) + ".level";
        }
    }

    // --- Write .garden file ---
    setPhase(PackageProgress::Phase::WritingManifest);

    json j;
    j["name"] = desc.name;
    j["engine_id"] = desc.engine_id;
    j["engine_version"] = desc.engine_version;
    j["game_module"] = desc.game_module;
    j["default_level"] = default_level;
    j["asset_directories"] = desc.asset_directories;

    fs::path garden_path = output_root / (desc.name + ".garden");
    std::ofstream garden_file(garden_path);
    if (!garden_file.is_open())
    {
        result.error_message = "Failed to write .garden file: " + garden_path.string();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    garden_file << j.dump(4);
    garden_file.close();
    result.files_copied++;

    LOG_ENGINE_INFO("[Packager] Package complete! {} files copied, {} levels compiled.",
                    result.files_copied, result.levels_compiled);
    LOG_ENGINE_INFO("[Packager] Output: {}", output_root.string());

    result.success = true;
    setPhase(PackageProgress::Phase::Complete);
    return result;
}

std::vector<std::string> ProjectPackager::validateBeforePackage(
    const ProjectManager& project_manager,
    const PackageConfig& config)
{
    std::vector<std::string> warnings;

    if (!project_manager.isLoaded())
        return warnings;

    const auto& desc = project_manager.getDescriptor();
    const fs::path project_root = project_manager.getProjectRoot();

    // Check game module DLL
    std::string module_path = project_manager.getAbsoluteModulePath();
    if (!module_path.empty() && !fs::exists(module_path))
        warnings.push_back("Game module DLL not found: " + module_path);

    // Check asset directories
    for (const auto& asset_dir : desc.asset_directories)
    {
        fs::path src = project_root / asset_dir;
        if (!fs::exists(src))
            warnings.push_back("Asset directory not found: " + src.string());
    }

    // Check default level
    if (!desc.default_level.empty())
    {
        fs::path level_path = project_root / desc.default_level;
        if (!fs::exists(level_path))
            warnings.push_back("Default level not found: " + desc.default_level);
    }

    // Check if output already exists
    if (!config.output_directory.empty() && !config.package_name.empty())
    {
        fs::path output_root = fs::path(config.output_directory) / config.package_name;
        if (fs::exists(output_root))
            warnings.push_back("Output directory already exists and will be overwritten");
    }

    return warnings;
}
