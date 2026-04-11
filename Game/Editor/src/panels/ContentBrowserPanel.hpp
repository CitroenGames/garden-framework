#pragma once

#include <string>
#include <vector>
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
    std::function<void(const std::string&)> on_preview_mesh;
    std::function<void(const std::string&)> on_open_prefab;

    // Set by EditorApp so we can trigger regeneration
    Assets::AssetScanner* asset_scanner = nullptr;

    void draw(bool* p_open = nullptr);

private:
    std::filesystem::path m_base_path = "assets";
    std::filesystem::path m_current_dir = "assets";
    int m_filter_mode = 0; // 0=All, 1=Levels, 2=Models, 3=Textures, 4=Shaders, 5=Prefabs
    bool m_grid_view = true;
    float m_thumbnail_size = 80.0f;

    // Selected mesh metadata display
    std::filesystem::path m_selected_mesh;
    Assets::AssetMetadata m_selected_metadata;
    bool m_has_selected_metadata = false;

    // Search
    char m_search_buf[256] = {0};

    // Sources panel toggle
    bool m_show_sources = true;

    // Inline rename state (files/folders)
    std::filesystem::path m_renaming_path;
    char m_rename_buf[256] = {0};
    bool m_rename_focus_set = false;

    // Deferred filesystem operations
    std::filesystem::path m_pending_delete;
    bool m_confirm_delete_open = false;

    // Navigation history (back/forward)
    std::vector<std::filesystem::path> m_nav_history;
    int m_nav_history_index = -1;
    bool m_nav_suppress_history = false;

    // Item counts (updated each frame)
    int m_visible_item_count = 0;

    // Tooltip cache
    std::filesystem::path m_tooltip_path;
    struct TooltipCache {
        std::string file_type;
        std::string file_size_str;
        int img_width = 0, img_height = 0;
        size_t vertex_count = 0;
        size_t triangle_count = 0;
        size_t lod_count = 0;
        bool has_metadata = false;
        bool loaded = false;
    } m_tooltip_cache;

    // Navigation
    void navigateTo(const std::filesystem::path& dir);
    void navigateBack();
    void navigateForward();

    // Drawing helpers
    void drawDirectoryTree(const std::filesystem::path& dir);
    void drawBreadcrumbBar();
    void drawStatusBar();
    void drawFileIcon(ImDrawList* draw_list, ImVec2 center, float size, const std::filesystem::path& path) const;
    void drawAssetTooltip(const std::filesystem::path& path);
    ImVec4 getFileColor(const std::filesystem::path& path) const;

    // Filtering
    bool passesFilter(const std::filesystem::path& path) const;
    bool passesSearchFilter(const std::filesystem::path& path) const;

    // File helpers
    void handleFileAction(const std::filesystem::path& path);
    bool isGeneratedFile(const std::filesystem::path& path) const;
    bool isMeshFile(const std::filesystem::path& path) const;
    bool isPrefabFile(const std::filesystem::path& path) const;
    bool isTextureFile(const std::filesystem::path& path) const;
    void drawMetadataStatusDot(ImDrawList* draw_list, ImVec2 pos, const std::filesystem::path& path) const;
    void drawMetadataInfo();
    void drawItemContextMenu(const std::filesystem::path& path);
    void drawBackgroundContextMenu();

    // File/folder creation helpers
    void createNewFolder(const std::filesystem::path& dir);
    void createEmptyPrefab(const std::filesystem::path& dir);
    void createEmptyLevel(const std::filesystem::path& dir);

    static std::string formatFileSize(uintmax_t bytes);
};
