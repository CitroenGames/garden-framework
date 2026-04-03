#pragma once

#include <string>
#include <vector>

struct ProjectDescriptor
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

// Manages .garden project files: loading, creating, and resolving paths.
class ProjectManager
{
public:
    ProjectManager() = default;

    // Load a .garden project file (JSON)
    bool loadProject(const std::string& project_file_path);

    // Create a new project from the built-in template
    bool createProject(const std::string& directory, const std::string& name);

    // Save current descriptor back to file
    bool saveProject();

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
