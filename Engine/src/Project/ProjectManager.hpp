#pragma once

#include "EngineExport.h"
#include <string>
#include <vector>

struct ENGINE_API ProjectDescriptor
{
    std::string name;
    std::string engine_id;         // Unique ID linking to a registered engine installation
    std::string engine_version;
    std::string game_module;       // e.g. "bin/MyGame" (no extension; resolved per-platform)
    std::string default_level;     // e.g. "assets/levels/main.level.json"
    std::vector<std::string> asset_directories;
    std::string source_directory;  // e.g. "src/"
    std::string buildscript;       // e.g. "MyGame.buildscript"
};

// Info about a discovered project template.
struct ENGINE_API TemplateInfo
{
    std::string name;          // from .garden "name" field
    std::string path;          // absolute path to template directory
    std::string garden_file;   // absolute path to the .garden file
};

// Manages .garden project files: loading, creating, and resolving paths.
class ENGINE_API ProjectManager
{
public:
    ProjectManager() = default;

    // Load a .garden project file (JSON)
    bool loadProject(const std::string& project_file_path);

    // Create a new project by copying an existing template directory.
    // All references to the template name are replaced with project_name.
    bool createProjectFromTemplate(const std::string& template_path,
                                   const std::string& destination_dir,
                                   const std::string& project_name);

    // Save current descriptor back to file
    bool saveProject();

    // Scan a directory for template subdirectories (each containing a .garden file)
    static std::vector<TemplateInfo> discoverTemplates(const std::string& templates_dir);

    // Accessors
    const ProjectDescriptor& getDescriptor() const { return m_descriptor; }
    ProjectDescriptor& getDescriptor() { return m_descriptor; }
    const std::string& getProjectRoot() const { return m_project_root; }
    const std::string& getProjectFilePath() const { return m_project_file_path; }
    bool isLoaded() const { return m_loaded; }

    // Resolve the game module path with platform-specific extension.
    // Returns absolute path: <project_root>/<game_module>.dll (or .so/.dylib)
    std::string getAbsoluteModulePath() const;

    // Resolve a project-relative path to absolute
    std::string resolveProjectPath(const std::string& relative) const;

private:
    ProjectDescriptor m_descriptor;
    std::string m_project_root;
    std::string m_project_file_path;
    bool m_loaded = false;
};
