#pragma once

#include "EngineExport.h"
#include "Graphics/RenderAPI.hpp"
#include "Assets/AssetCompiler.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>

class ProjectManager;
class LevelManager;

struct ENGINE_API PackageConfig
{
    std::string output_directory;
    std::string package_name;
    bool compile_levels_to_binary = false;
    RenderAPIType target_render_api = DefaultRenderAPI;

    // Asset compilation
    bool compile_assets = false;
    Assets::CompileConfig compile_config;
};

struct ENGINE_API PackageResult
{
    bool success = false;
    std::string error_message;
    int files_copied = 0;
    int levels_compiled = 0;
    int models_compiled = 0;
    int textures_compiled = 0;
    int assets_skipped = 0;
    std::vector<std::string> warnings;
};

struct ENGINE_API PackageProgress
{
    enum class Phase {
        CopyingBinaries,
        CompilingAssets,
        ValidatingAssets,
        CompilingLevels,
        WritingManifest,
        Complete,
        Failed
    };

    std::atomic<Phase> current_phase{Phase::CopyingBinaries};

    // Asset compilation counters (updated from worker threads)
    std::atomic<int> total_assets{0};
    std::atomic<int> completed_assets{0};
    std::atomic<int> skipped_assets{0};
    std::atomic<int> failed_assets{0};

    std::mutex current_asset_mutex;
    std::string current_asset; // protected by current_asset_mutex

    // Result — only valid after finished == true
    std::atomic<bool> finished{false};
    PackageResult result;
};

class ENGINE_API ProjectPackager
{
public:
    static PackageResult packageProject(
        const ProjectManager& project_manager,
        LevelManager& level_manager,
        const PackageConfig& config);

    // Launches packaging on a background thread. Poll progress->finished for completion.
    static void packageProjectAsync(
        const ProjectManager& project_manager,
        LevelManager& level_manager,
        const PackageConfig& config,
        std::shared_ptr<PackageProgress> progress);

    static std::vector<std::string> validateBeforePackage(
        const ProjectManager& project_manager,
        const PackageConfig& config);

private:
    static PackageResult packageProjectInternal(
        const ProjectManager& project_manager,
        LevelManager& level_manager,
        const PackageConfig& config,
        std::shared_ptr<PackageProgress> progress);
};
