#include "ProjectPackager.hpp"
#include "ProjectManager.hpp"
#include "LevelManager.hpp"
#include "Assets/AssetManager.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <exception>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string shaderSubdir(RenderAPIType api)
{
    switch (api)
    {
    case RenderAPIType::D3D12:  return "d3d12";
    case RenderAPIType::Vulkan: return "vulkan";
    case RenderAPIType::Metal:  return "metal";
    default:                    return "d3d12";
    }
}

static int copyDirectoryRecursive(const fs::path& src, const fs::path& dst,
                                  std::string* error_out = nullptr)
{
    int count = 0;
    if (error_out) error_out->clear();
    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        LOG_ENGINE_ERROR("[Packager] {}", message);
        return count;
    };

    std::error_code ec;
    if (!fs::is_directory(src, ec) || ec)
    {
        const std::string reason = ec ? ec.message() : "not a directory";
        return fail("Asset directory is unavailable '" + src.string() + "': " + reason);
    }

    fs::create_directories(dst, ec);
    if (ec)
        return fail("Failed to create directory '" + dst.string() + "': " + ec.message());

    fs::recursive_directory_iterator it(src, ec);
    const fs::recursive_directory_iterator end;
    if (ec)
        return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

    for (; it != end; it.increment(ec))
    {
        if (ec)
            return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

        const fs::path relative = it->path().lexically_relative(src);
        if (relative.empty())
            return fail("Failed to resolve asset path relative to '" + src.string() + "': " + it->path().string());

        const fs::path dest_path = dst / relative;
        const auto status = it->status(ec);
        if (ec)
            return fail("Failed to inspect asset '" + it->path().string() + "': " + ec.message());

        if (fs::is_directory(status))
        {
            fs::create_directories(dest_path, ec);
            if (ec)
                return fail("Failed to create directory '" + dest_path.string() + "': " + ec.message());
        }
        else if (fs::is_regular_file(status))
        {
            fs::create_directories(dest_path.parent_path(), ec);
            if (ec)
                return fail("Failed to create directory '" + dest_path.parent_path().string() + "': " + ec.message());

            fs::copy_file(it->path(), dest_path, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return fail("Failed to copy asset '" + it->path().string() + "' to '" + dest_path.string() + "': " + ec.message());
            ++count;
        }
        else
        {
            return fail("Unsupported asset entry '" + it->path().string() + "'");
        }
    }

    if (ec)
        return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

    if (error_out) error_out->clear();
    return count;
}

static bool copySingleFile(const fs::path& src, const fs::path& dst, const char* label,
                           std::string* error_out = nullptr)
{
    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        LOG_ENGINE_ERROR("[Packager] {}", message);
        return false;
    };

    std::error_code ec;
    if (!fs::is_regular_file(src, ec) || ec)
    {
        const std::string reason = ec ? ec.message() : "not a regular file";
        return fail(std::string(label) + " not found: " + src.string() + " (" + reason + ")");
    }

    fs::create_directories(dst.parent_path(), ec);
    if (ec)
        return fail("Failed to create directory for " + std::string(label) + ": " + ec.message());

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return fail(std::string("Failed to copy ") + label + " from '" + src.string() + "' to '" + dst.string() + "': " + ec.message());

    if (error_out) error_out->clear();
    return true;
}

static bool isCompiledMeshSource(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".gltf" || extension == ".glb" || extension == ".obj";
}

static bool isCompiledTextureSource(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".bmp" || extension == ".tga";
}

static bool startsWithAssetDirectory(const fs::path& asset_path, const fs::path& asset_directory)
{
    std::string path = asset_path.lexically_normal().generic_string();
    std::string directory = asset_directory.lexically_normal().generic_string();
    while (!path.empty() && path.back() == '/') path.pop_back();
    while (!directory.empty() && directory.back() == '/') directory.pop_back();
    if (directory.empty() || directory == "." || path.size() < directory.size())
        return false;

#ifdef _WIN32
    const bool prefix_matches = std::equal(directory.begin(), directory.end(), path.begin(),
        [](unsigned char lhs, unsigned char rhs) { return std::tolower(lhs) == std::tolower(rhs); });
#else
    const bool prefix_matches = path.compare(0, directory.size(), directory) == 0;
#endif
    return prefix_matches && (path.size() == directory.size() || path[directory.size()] == '/');
}

static bool packagedAssetExists(const fs::path& output_root, const std::string& asset_path,
                                const std::vector<std::string>& asset_directories,
                                bool compile_assets, const Assets::CompileConfig& compile_config)
{
    const fs::path reference_path(asset_path);
    std::vector<fs::path> source_paths{ output_root / reference_path };
    const bool project_relative = reference_path.is_absolute() ||
        std::any_of(asset_directories.begin(), asset_directories.end(), [&](const std::string& directory) {
            return startsWithAssetDirectory(reference_path, fs::path(directory));
        });
    if (!project_relative)
    {
        for (const auto& directory : asset_directories)
            source_paths.push_back(output_root / directory / reference_path);
    }

    for (const auto& source_path : source_paths)
    {
        std::error_code ec;
        if (compile_assets && isCompiledMeshSource(source_path) && compile_config.compile_models)
        {
            fs::path compiled_path = source_path;
            compiled_path.replace_extension(".cmesh");
            if (fs::exists(compiled_path, ec) && !ec)
                return true;
            continue;
        }
        if (compile_assets && isCompiledTextureSource(source_path) && compile_config.compile_textures)
        {
            fs::path compiled_path = source_path;
            compiled_path.replace_extension(".ctex");
            if (fs::exists(compiled_path, ec) && !ec)
                return true;
            continue;
        }

        // Unsupported formats and formats explicitly excluded from compilation
        // pass through AssetCompiler as raw files.
        if (fs::exists(source_path, ec) && !ec)
            return true;
    }

    return false;
}

std::vector<std::string> ProjectPackager::requiredEngineBinaries(bool include_dedicated_server)
{
#ifdef _WIN32
    std::vector<std::string> files = { "Game.exe", "EngineCore.dll", "EngineGraphics.dll", "SDL3.dll" };
    if (include_dedicated_server) files.push_back("Server.exe");
#elif defined(__APPLE__)
    std::vector<std::string> files = { "Game", "libEngineCore.dylib", "libEngineGraphics.dylib" };
    if (include_dedicated_server) files.push_back("Server");
#else
    std::vector<std::string> files = { "Game", "libEngineCore.so", "libEngineGraphics.so" };
    if (include_dedicated_server) files.push_back("Server");
#endif
    return files;
}

std::vector<std::string> ProjectPackager::missingEngineBinaries(const std::string& engine_bin_dir, bool include_dedicated_server)
{
    std::vector<std::string> missing;
    for (const auto& file : requiredEngineBinaries(include_dedicated_server))
    {
        if (!fs::exists(fs::path(engine_bin_dir) / file))
            missing.push_back(file);
    }
    return missing;
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
    if (module_path.empty())
    {
        result.error_message = "Game module is not configured in the project.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    std::error_code module_ec;
    if (!fs::is_regular_file(module_path, module_ec) || module_ec)
    {
        result.error_message = "Game module DLL not found: " + module_path +
            "\nBuild the game module before packaging.";
        if (module_ec)
            result.error_message += " (" + module_ec.message() + ")";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Project content directories are required package inputs. Validate all of
    // them before creating output or copying engine files.
    for (const auto& asset_dir : desc.asset_directories)
    {
        const fs::path src = project_root / asset_dir;
        std::error_code asset_ec;
        if (!fs::is_directory(src, asset_ec) || asset_ec)
        {
            result.error_message = "Asset directory not found: " + src.string();
            if (asset_ec)
                result.error_message += " (" + asset_ec.message() + ")";
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
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

    const std::vector<std::string> engine_files = requiredEngineBinaries(config.include_dedicated_server);

    for (const auto& file : engine_files)
    {
        std::string copy_error;
        if (copySingleFile(engine_bin / file, output_root / "bin" / file, file.c_str(), &copy_error))
            result.files_copied++;
        else
        {
            result.error_message = "Missing required engine binary '" + file + "': " + copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // --- Copy game module DLL ---
    fs::path module_filename = fs::path(module_path).filename();
    std::string module_copy_error;
    if (!copySingleFile(module_path, output_root / "bin" / module_filename, "Game module", &module_copy_error))
    {
        result.error_message = module_copy_error;
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    result.files_copied++;

    // --- Copy engine assets ---
    fs::path engine_assets = fs::path(engine_bin) / ".." / "assets";
    engine_assets = fs::weakly_canonical(engine_assets);

    // Shaders – copy all platform variants available on this OS
#ifdef _WIN32
    const std::vector<std::string> shader_dirs = { "d3d12", "vulkan" };
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
            std::string copy_error;
            result.files_copied += copyDirectoryRecursive(shader_src, shader_dst, &copy_error);
            if (!copy_error.empty())
            {
                result.error_message = copy_error;
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }
        }
    }

    // Fonts
    fs::path fonts_src = engine_assets / "fonts";
    if (fs::exists(fonts_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying fonts...");
        std::string copy_error;
        result.files_copied += copyDirectoryRecursive(fonts_src, output_root / "assets" / "fonts", &copy_error);
        if (!copy_error.empty())
        {
            result.error_message = copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // UI
    fs::path ui_src = engine_assets / "ui";
    if (fs::exists(ui_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying UI assets...");
        std::string copy_error;
        result.files_copied += copyDirectoryRecursive(ui_src, output_root / "assets" / "ui", &copy_error);
        if (!copy_error.empty())
        {
            result.error_message = copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // --- Copy/compile project assets ---
    setPhase(PackageProgress::Phase::CompilingAssets);

    for (const auto& asset_dir : desc.asset_directories)
    {
        fs::path src = project_root / asset_dir;
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

            std::string compile_error;
            if (!compile_result.errors.empty())
                compile_error = compile_result.errors.front();
            else
            {
                for (const auto& warning : compile_result.warnings)
                {
                    if (warning.rfind("Failed to copy:", 0) == 0)
                    {
                        compile_error = warning;
                        break;
                    }
                }
            }

            if (compile_result.failed_assets > 0 || !compile_error.empty())
            {
                result.error_message = "Project asset compilation failed";
                if (!compile_error.empty())
                    result.error_message += ": " + compile_error;
                else
                    result.error_message += ": " + std::to_string(compile_result.failed_assets) + " asset(s) failed";
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }

            LOG_ENGINE_INFO("[Packager] Asset compilation complete: {} models, {} textures, {} skipped",
                            compile_result.models_compiled, compile_result.textures_compiled, compile_result.skipped_assets);
        }
        else
        {
            LOG_ENGINE_INFO("[Packager] Copying project assets from '{}'...", asset_dir);
            std::string copy_error;
            result.files_copied += copyDirectoryRecursive(src, dst, &copy_error);
            if (!copy_error.empty())
            {
                result.error_message = copy_error;
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }
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
            for (const auto& asset_path : referenced)
            {
                if (!packagedAssetExists(output_root, asset_path, desc.asset_directories,
                                          config.compile_assets, config.compile_config))
                {
                    result.error_message = "Referenced asset missing from package: " + asset_path;
                    LOG_ENGINE_ERROR("[Packager] {}", result.error_message);
                    setPhase(PackageProgress::Phase::Failed);
                    return result;
                }
            }
            if (!referenced.empty())
                LOG_ENGINE_INFO("[Packager] All {} referenced assets validated", referenced.size());
        }
    }

    // --- Optionally compile levels to binary ---
    std::string default_level = desc.default_level;
    bool default_level_compiled = false;

    if (config.compile_levels_to_binary)
    {
        setPhase(PackageProgress::Phase::CompilingLevels);
        LOG_ENGINE_INFO("[Packager] Compiling levels to binary...");

        // Find all .level.json files in the output
        fs::recursive_directory_iterator level_it(output_root / "assets", ec);
        const fs::recursive_directory_iterator level_end;
        if (ec)
        {
            result.error_message = "Failed to enumerate package assets for level compilation: " + ec.message();
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        for (; level_it != level_end; level_it.increment(ec))
        {
            if (ec)
            {
                result.error_message = "Failed to enumerate package assets for level compilation: " + ec.message();
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }

            const auto& entry = *level_it;
            std::error_code status_ec;
            if (!entry.is_regular_file(status_ec))
            {
                if (status_ec)
                {
                    result.error_message = "Failed to inspect packaged level candidate '" + entry.path().string() + "': " + status_ec.message();
                    setPhase(PackageProgress::Phase::Failed);
                    return result;
                }
                continue;
            }

            std::string filename = entry.path().filename().string();
            if (filename.size() > 11 && filename.substr(filename.size() - 11) == ".level.json")
            {
                std::string json_path = entry.path().string();
                std::string binary_path = json_path.substr(0, json_path.size() - 11) + ".level";

                bool level_compiled = false;
                try
                {
                    level_compiled = level_manager.compileLevel(json_path, binary_path);
                }
                catch (const std::exception& e)
                {
                    result.error_message = "Failed to compile level: " + filename + ": " + e.what();
                    LOG_ENGINE_ERROR("[Packager] {}", result.error_message);
                    setPhase(PackageProgress::Phase::Failed);
                    return result;
                }
                catch (...)
                {
                    result.error_message = "Failed to compile level: " + filename + ": unexpected exception";
                    LOG_ENGINE_ERROR("[Packager] {}", result.error_message);
                    setPhase(PackageProgress::Phase::Failed);
                    return result;
                }

                if (level_compiled)
                {
                    // Remove the JSON original
                    std::error_code remove_ec;
                    if (!fs::remove(entry.path(), remove_ec) || remove_ec)
                    {
                        result.error_message = "Failed to remove source level after compilation '" + entry.path().string() + "': " +
                            (remove_ec ? remove_ec.message() : "file was not removed");
                        setPhase(PackageProgress::Phase::Failed);
                        return result;
                    }

                    ++result.levels_compiled;
                    if (entry.path().lexically_relative(output_root).lexically_normal() == fs::path(desc.default_level).lexically_normal())
                        default_level_compiled = true;
                    LOG_ENGINE_INFO("[Packager] Compiled: {}", filename);
                }
                else
                {
                    std::string msg = "Failed to compile level: " + filename;
                    result.warnings.push_back(msg);
                    result.error_message = msg;
                    LOG_ENGINE_ERROR("[Packager] {}", msg);
                    setPhase(PackageProgress::Phase::Failed);
                    return result;
                }
            }
        }
        if (ec)
        {
            result.error_message = "Failed to enumerate package assets for level compilation: " + ec.message();
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }

        // Update default_level extension
        if (default_level.size() > 11 &&
            default_level.substr(default_level.size() - 11) == ".level.json")
        {
            if (!default_level_compiled)
            {
                result.error_message = "Default level was not compiled: " + default_level;
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }
            default_level = default_level.substr(0, default_level.size() - 11) + ".level";
        }
    }

    // --- Write .garden file ---
    setPhase(PackageProgress::Phase::WritingManifest);

    json j;
    std::ifstream source_manifest(project_manager.getProjectFilePath());
    if (!source_manifest.is_open())
    {
        result.error_message = "Failed to read source .garden file: " + project_manager.getProjectFilePath();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    try
    {
        source_manifest >> j;
    }
    catch (const std::exception& e)
    {
        result.error_message = "Failed to parse source .garden file: " + std::string(e.what());
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    if (!j.is_object())
    {
        result.error_message = "Source .garden file must contain a JSON object.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Update the fields understood by this engine while retaining additive
    // fields written by newer tools or custom project pipelines.
    j["name"] = desc.name;
    j["engine_id"] = desc.engine_id;
    j["engine_version"] = desc.engine_version;
    j["game_module"] = desc.game_module;
    j["default_level"] = default_level;
    j["asset_directories"] = desc.asset_directories;
    j["default_game_mode"] = desc.default_game_mode;
    j["default_game_state"] = desc.default_game_state;
    j["source_directory"] = desc.source_directory;
    j["buildscript"] = desc.buildscript;

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
    if (!garden_file)
    {
        result.error_message = "Failed to finish writing .garden file: " + garden_path.string();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
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
