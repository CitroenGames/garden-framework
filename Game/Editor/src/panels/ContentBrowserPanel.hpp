#pragma once

#include <string>
#include <functional>
#include <filesystem>
#include "imgui.h"
#include "Assets/AssetMetadata.hpp"
#include "Assets/AssetMetadataSerializer.hpp"

namespace Assets { class AssetScanner; }

class ContentBrowserPanel
{
public:
    std::function<void(const std::string&)> on_open_level;
    std::function<void(const std::string&)> on_open_mesh;

    // Set by EditorApp so we can trigger regeneration
    Assets::AssetScanner* asset_scanner = nullptr;

    void draw();

private:
    std::filesystem::path m_base_path = "assets";
    std::filesystem::path m_current_dir = "assets";
    int m_filter_mode = 0; // 0=All, 1=Levels, 2=Models, 3=Textures, 4=Shaders
    bool m_grid_view = true;
    float m_thumbnail_size = 80.0f;

    // Selected mesh metadata display
    std::filesystem::path m_selected_mesh;
    Assets::AssetMetadata m_selected_metadata;
    bool m_has_selected_metadata = false;

    void drawDirectoryTree(const std::filesystem::path& dir);
    void drawBreadcrumbs();
    void drawFileIcon(ImDrawList* draw_list, ImVec2 center, float size, const std::filesystem::path& path) const;
    ImVec4 getFileColor(const std::filesystem::path& path) const;
    bool passesFilter(const std::filesystem::path& path) const;
    void handleFileAction(const std::filesystem::path& path);
    bool isGeneratedFile(const std::filesystem::path& path) const;
    bool isMeshFile(const std::filesystem::path& path) const;
    void drawMetadataStatusDot(ImDrawList* draw_list, ImVec2 pos, const std::filesystem::path& path) const;
    void drawMetadataInfo();
    void drawContextMenu(const std::filesystem::path& path);
};
