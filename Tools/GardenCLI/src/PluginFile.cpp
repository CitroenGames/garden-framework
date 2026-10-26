#include "PluginFile.hpp"
#include "json.hpp"

#include <fstream>
#include <cstdio>

using json = nlohmann::json;

bool loadGardenPlugin(const std::string& path, GardenPlugin& out)
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

    out.name             = j.value("name", std::string());
    out.version          = j.value("version", std::string());
    out.engine_id        = j.value("engine_id", std::string());
    out.engine_version   = j.value("engine_version", std::string("1.0"));
    out.min_editor_api   = j.value("min_editor_api", 0);
    out.author           = j.value("author", std::string());
    out.description      = j.value("description", std::string());
    out.buildscript      = j.value("buildscript", std::string());
    out.source_directory = j.value("source_directory", std::string("src/"));
    out.output_dll       = j.value("output_dll", out.name);
    out.enabled          = j.value("enabled", true);

    out.tags.clear();
    if (j.contains("tags") && j["tags"].is_array())
    {
        for (auto& t : j["tags"])
            if (t.is_string())
                out.tags.push_back(t.get<std::string>());
    }

    if (out.buildscript.empty())
    {
        fprintf(stderr, "Error: '%s' is missing required field 'buildscript'\n", path.c_str());
        return false;
    }
    if (out.name.empty())
    {
        fprintf(stderr, "Error: '%s' is missing required field 'name'\n", path.c_str());
        return false;
    }

    return true;
}

bool setPluginEngineId(const std::string& path, const std::string& engine_id)
{
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

    j["engine_id"] = engine_id;

    std::ofstream file(path);
    if (!file.is_open())
    {
        fprintf(stderr, "Error: Cannot write to '%s'\n", path.c_str());
        return false;
    }
    file << j.dump(4);
    return true;
}
