#include "ProjectFile.hpp"
#include "json.hpp"

#include <fstream>
#include <cstdio>

using json = nlohmann::json;

bool loadGardenProject(const std::string& path, GardenProject& out)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        fprintf(stderr, "Error: Cannot open '%s'\n", path.c_str());
        return false;
    }

    json j;
    try
    {
        file >> j;
    }
    catch (const json::parse_error& e)
    {
        fprintf(stderr, "Error: Invalid JSON in '%s': %s\n", path.c_str(), e.what());
        return false;
    }

    out.name = j.value("name", "Untitled");
    out.engine_id = j.value("engine_id", "");
    out.engine_version = j.value("engine_version", "");
    out.default_level = j.value("default_level", "");
    out.game_module = j.value("game_module", "");
    out.buildscript = j.value("buildscript", "");
    return true;
}

bool setProjectEngineId(const std::string& path, const std::string& engine_id)
{
    // Read existing JSON
    json j;
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            fprintf(stderr, "Error: Cannot open '%s'\n", path.c_str());
            return false;
        }
        try { file >> j; }
        catch (const json::parse_error& e)
        {
            fprintf(stderr, "Error: Invalid JSON in '%s': %s\n", path.c_str(), e.what());
            return false;
        }
    }

    // Update engine_id
    j["engine_id"] = engine_id;

    // Write back
    std::ofstream file(path);
    if (!file.is_open())
    {
        fprintf(stderr, "Error: Cannot write to '%s'\n", path.c_str());
        return false;
    }
    file << j.dump(4);
    return true;
}
