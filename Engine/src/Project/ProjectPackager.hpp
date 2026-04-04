#pragma once

#include "EngineExport.h"
#include "Graphics/RenderAPI.hpp"
#include <string>

class ProjectManager;
class LevelManager;

struct ENGINE_API PackageConfig
{
    std::string output_directory;
    std::string package_name;
    bool compile_levels_to_binary = false;
    RenderAPIType target_render_api = DefaultRenderAPI;
};

struct ENGINE_API PackageResult
{
    bool success = false;
    std::string error_message;
    int files_copied = 0;
    int levels_compiled = 0;
};

class ENGINE_API ProjectPackager
{
public:
    static PackageResult packageProject(
        const ProjectManager& project_manager,
        LevelManager& level_manager,
        const PackageConfig& config);
};
