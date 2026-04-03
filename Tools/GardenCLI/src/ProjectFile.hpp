#pragma once

#include <string>

struct GardenProject
{
    std::string name;
    std::string engine_id;
    std::string engine_version;
    std::string default_level;
    std::string game_module;
};

// Lightweight .garden file reader — no Engine dependency.
bool loadGardenProject(const std::string& path, GardenProject& out);

// Set the engine_id field in a .garden file (preserves all other fields).
bool setProjectEngineId(const std::string& path, const std::string& engine_id);
