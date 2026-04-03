#pragma once

#include <string>
#include <functional>
#include <filesystem>
#include "imgui.h"

class ContentBrowserPanel
{
public:
    std::function<void(const std::string&)> on_open_level;

    void draw();

private:
    std::filesystem::path m_base_path = "assets";
    std::filesystem::path m_current_dir = "assets";
    int m_filter_mode = 0; // 0=All, 1=Levels, 2=Models, 3=Textures, 4=Shaders
    bool m_grid_view = true;
    float m_thumbnail_size = 80.0f;

    void drawDirectoryTree(const std::filesystem::path& dir);
    void drawBreadcrumbs();
    void drawFileIcon(ImDrawList* draw_list, ImVec2 center, float size, const std::filesystem::path& path) const;
    ImVec4 getFileColor(const std::filesystem::path& path) const;
    bool passesFilter(const std::filesystem::path& path) const;
    void handleFileAction(const std::filesystem::path& path);
};
