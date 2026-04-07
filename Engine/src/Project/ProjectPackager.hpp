#pragma once

#include "EngineExport.h"
#include "Graphics/RenderAPI.hpp"
#include "Assets/AssetCompiler.hpp"
#include <string>
#include <vector>

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

class ENGINE_API ProjectPackager
{
public:
    static PackageResult packageProject(
        const ProjectManager& project_manager,
        LevelManager& level_manager,
        const PackageConfig& config);

    static std::vector<std::string> validateBeforePackage(
        const ProjectManager& project_manager,
        const PackageConfig& config);
};
