#include "ProjectManager.hpp"
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <regex>

// nlohmann json is available via tinygltf
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

bool ProjectManager::loadProject(const std::string& project_file_path)
{
    m_loaded = false;

    std::ifstream file(project_file_path);
    if (!file.is_open())
    {
        fprintf(stderr, "[ProjectManager] Cannot open '%s'\n", project_file_path.c_str());
        return false;
    }

    json j;
    try
    {
        file >> j;
    }
    catch (const json::parse_error& e)
    {
        fprintf(stderr, "[ProjectManager] JSON parse error in '%s': %s\n",
                project_file_path.c_str(), e.what());
        return false;
    }

    m_project_file_path = fs::absolute(project_file_path).string();
    m_project_root = fs::path(m_project_file_path).parent_path().string();

    m_descriptor.name = j.value("name", "Untitled");
    m_descriptor.engine_id = j.value("engine_id", "");
    m_descriptor.engine_version = j.value("engine_version", "1.0");
    m_descriptor.game_module = j.value("game_module", "");
    m_descriptor.default_level = j.value("default_level", "");
    m_descriptor.source_directory = j.value("source_directory", "src/");
    m_descriptor.buildscript = j.value("buildscript", "");

    m_descriptor.asset_directories.clear();
    if (j.contains("asset_directories") && j["asset_directories"].is_array())
    {
        for (auto& dir : j["asset_directories"])
            m_descriptor.asset_directories.push_back(dir.get<std::string>());
    }
    if (m_descriptor.asset_directories.empty())
        m_descriptor.asset_directories.push_back("assets/");

    m_loaded = true;
    printf("[ProjectManager] Loaded project '%s' from '%s'\n",
           m_descriptor.name.c_str(), project_file_path.c_str());
    return true;
}

bool ProjectManager::createProject(const std::string& directory, const std::string& name)
{
    fs::path project_dir = fs::path(directory) / name;

    std::error_code ec;
    fs::create_directories(project_dir, ec);
    fs::create_directories(project_dir / "src" / "Components", ec);
    fs::create_directories(project_dir / "assets" / "levels", ec);
    fs::create_directories(project_dir / "assets" / "models", ec);
    fs::create_directories(project_dir / "assets" / "shaders", ec);
    fs::create_directories(project_dir / "assets" / "textures", ec);
    fs::create_directories(project_dir / "bin", ec);

    // Write .garden project file
    json j;
    j["name"] = name;
    j["engine_id"] = "";
    j["engine_version"] = "1.0";
    j["game_module"] = "bin/" + name;
    j["default_level"] = "assets/levels/main.level.json";
    j["asset_directories"] = { "assets/" };
    j["source_directory"] = "src/";
    j["buildscript"] = name + ".buildscript";

    fs::path garden_path = project_dir / (name + ".garden");
    std::ofstream garden_file(garden_path);
    if (!garden_file.is_open())
    {
        fprintf(stderr, "[ProjectManager] Cannot write '%s'\n", garden_path.string().c_str());
        return false;
    }
    garden_file << j.dump(4);
    garden_file.close();

    // Write minimal GameModule.cpp
    fs::path module_cpp = project_dir / "src" / "GameModule.cpp";
    std::ofstream src_file(module_cpp);
    if (src_file.is_open())
    {
        src_file << R"(#include "Plugin/GameModuleAPI.h"
#include "Reflection/Reflect.hpp"
#include "Reflection/ReflectionRegistry.hpp"

static EngineServices* g_services = nullptr;

GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}

GAME_API const char* gardenGetGameName()
{
    return ")" << name << R"(";
}

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    return true;
}

GAME_API void gardenGameShutdown()
{
    g_services = nullptr;
}

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
    // Register your game components here:
    // registerReflection_MyComponent(*registry);
}

GAME_API void gardenGameUpdate(float delta_time)
{
    // Game logic per frame
}

GAME_API void gardenOnLevelLoaded()
{
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
)";
        src_file.close();
    }

    // Write buildscript
    fs::path buildscript_path = project_dir / (name + ".buildscript");
    std::ofstream bs_file(buildscript_path);
    if (bs_file.is_open())
    {
        bs_file << "[solution]\n";
        bs_file << "name = " << name << "\n";
        bs_file << "platforms = x64, Linux\n";
        bs_file << "\n";
        bs_file << "include = ${ENGINE_PATH}/Engine/Engine.buildscript\n";
        bs_file << "\n";
        bs_file << "[project:" << name << "]\n";
        bs_file << "type = dll\n";
        bs_file << "sources = src/**/*.cpp\n";
        bs_file << "headers = src/**/*.hpp\n";
        bs_file << "includes = src\n";
        bs_file << "target_link_libraries(\n";
        bs_file << "    PRIVATE EngineGraphics\n";
        bs_file << ")\n";
        bs_file << "std = 20\n";
        bs_file << "utf8 = true\n";
        bs_file.close();
    }

    // Write empty default level
    fs::path level_path = project_dir / "assets" / "levels" / "main.level.json";
    std::ofstream level_file(level_path);
    if (level_file.is_open())
    {
        json level;
        level["metadata"] = {
            {"level_name", "Main Level"},
            {"author", ""},
            {"version", "1.0"}
        };
        level["entities"] = json::array();
        level_file << level.dump(4);
        level_file.close();
    }

    // Now load the project we just created
    return loadProject(garden_path.string());
}

bool ProjectManager::createProjectFromTemplate(const std::string& template_path,
                                                const std::string& destination_dir,
                                                const std::string& project_name)
{
    fs::path src_dir = fs::path(template_path);
    fs::path dst_dir = fs::path(destination_dir) / project_name;

    // Find the .garden file in the template to detect the original name
    std::string original_name;
    std::string garden_filename;
    for (const auto& entry : fs::directory_iterator(src_dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".garden")
        {
            garden_filename = entry.path().filename().string();

            std::ifstream f(entry.path());
            if (f.is_open())
            {
                try {
                    json j;
                    f >> j;
                    original_name = j.value("name", "");
                } catch (...) {}
            }
            break;
        }
    }

    if (original_name.empty())
    {
        fprintf(stderr, "[ProjectManager] Template at '%s' has no valid .garden file\n",
                template_path.c_str());
        return false;
    }

    // Copy entire template directory to destination
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    fs::copy(src_dir, dst_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        fprintf(stderr, "[ProjectManager] Failed to copy template: %s\n", ec.message().c_str());
        return false;
    }

    // Rename files: original_name.garden -> project_name.garden, etc.
    fs::path old_garden = dst_dir / (original_name + ".garden");
    fs::path new_garden = dst_dir / (project_name + ".garden");
    if (fs::exists(old_garden))
        fs::rename(old_garden, new_garden, ec);

    fs::path old_bs = dst_dir / (original_name + ".buildscript");
    fs::path new_bs = dst_dir / (project_name + ".buildscript");
    if (fs::exists(old_bs))
        fs::rename(old_bs, new_bs, ec);

    // Helper: replace all occurrences of original_name with project_name in a file
    auto replaceInFile = [&](const fs::path& file_path) {
        std::ifstream in(file_path);
        if (!in.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        in.close();

        // Replace all occurrences
        std::string::size_type pos = 0;
        while ((pos = content.find(original_name, pos)) != std::string::npos)
        {
            content.replace(pos, original_name.length(), project_name);
            pos += project_name.length();
        }

        std::ofstream out(file_path);
        if (out.is_open())
            out << content;
    };

    // Update file contents
    replaceInFile(new_garden);
    replaceInFile(new_bs);

    // Update GameModule.cpp (gardenGetGameName return value)
    fs::path module_cpp = dst_dir / "src" / "GameModule.cpp";
    if (fs::exists(module_cpp))
        replaceInFile(module_cpp);

    printf("[ProjectManager] Created project '%s' from template '%s'\n",
           project_name.c_str(), original_name.c_str());

    return loadProject(new_garden.string());
}

std::vector<TemplateInfo> ProjectManager::discoverTemplates(const std::string& templates_dir)
{
    std::vector<TemplateInfo> templates;

    if (!fs::exists(templates_dir) || !fs::is_directory(templates_dir))
        return templates;

    for (const auto& entry : fs::directory_iterator(templates_dir))
    {
        if (!entry.is_directory())
            continue;

        // Look for a .garden file in this subdirectory
        for (const auto& file : fs::directory_iterator(entry.path()))
        {
            if (file.is_regular_file() && file.path().extension() == ".garden")
            {
                TemplateInfo info;
                info.path = fs::absolute(entry.path()).string();
                info.garden_file = fs::absolute(file.path()).string();

                // Parse the .garden file to get the name
                std::ifstream f(file.path());
                if (f.is_open())
                {
                    try {
                        json j;
                        f >> j;
                        info.name = j.value("name", entry.path().filename().string());
                    } catch (...) {
                        info.name = entry.path().filename().string();
                    }
                }
                else
                {
                    info.name = entry.path().filename().string();
                }

                templates.push_back(info);
                break; // Only one .garden per template directory
            }
        }
    }

    return templates;
}

bool ProjectManager::saveProject()
{
    if (!m_loaded || m_project_file_path.empty())
        return false;

    json j;
    j["name"] = m_descriptor.name;
    j["engine_id"] = m_descriptor.engine_id;
    j["engine_version"] = m_descriptor.engine_version;
    j["game_module"] = m_descriptor.game_module;
    j["default_level"] = m_descriptor.default_level;
    j["asset_directories"] = m_descriptor.asset_directories;
    j["source_directory"] = m_descriptor.source_directory;
    j["buildscript"] = m_descriptor.buildscript;

    std::ofstream file(m_project_file_path);
    if (!file.is_open())
        return false;

    file << j.dump(4);
    return true;
}

std::string ProjectManager::getAbsoluteModulePath() const
{
    if (!m_loaded || m_descriptor.game_module.empty())
        return "";

    std::string module = m_descriptor.game_module;

#ifdef _WIN32
    module += ".dll";
#elif defined(__APPLE__)
    // Prepend lib if not already present
    fs::path p(module);
    if (p.filename().string().substr(0, 3) != "lib")
        module = (p.parent_path() / ("lib" + p.filename().string())).string();
    module += ".dylib";
#else
    fs::path p(module);
    if (p.filename().string().substr(0, 3) != "lib")
        module = (p.parent_path() / ("lib" + p.filename().string())).string();
    module += ".so";
#endif

    return (fs::path(m_project_root) / module).string();
}

std::string ProjectManager::resolveProjectPath(const std::string& relative) const
{
    return (fs::path(m_project_root) / relative).string();
}
