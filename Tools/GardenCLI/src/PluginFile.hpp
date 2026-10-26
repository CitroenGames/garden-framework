#pragma once

#include <string>
#include <vector>

// Schema for `.gardenplugin` files. Mirrors GardenProject in shape but covers
// editor plugins instead of game projects:
//   - `engine_id` links to a registered engine just like .garden does
//   - `buildscript` is what GardenCLI hands to sighmake on `generate-plugin`
//   - the runtime fields (name/version/author/description/min_editor_api/tags)
//     are the SAME data the editor's EditorPluginHost reads to populate its
//     Plugin Manager UI — having them in the same file removes the previous
//     plugin.json / .buildscript split.
struct GardenPlugin
{
    std::string name;
    std::string version;
    std::string engine_id;
    std::string engine_version;
    int         min_editor_api = 0;
    std::string author;
    std::string description;
    std::string buildscript;
    std::string source_directory;
    std::string output_dll;        // DLL stem (no extension), defaults to `name`
    std::vector<std::string> tags;
    bool        enabled = true;
};

// Lightweight `.gardenplugin` reader — no Engine dependency.
bool loadGardenPlugin(const std::string& path, GardenPlugin& out);

// Update the `engine_id` field while preserving every other field. Same
// pattern as setProjectEngineId in ProjectFile.
bool setPluginEngineId(const std::string& path, const std::string& engine_id);
