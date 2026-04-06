#include "ContentBrowserPanel.hpp"
#include "Assets/AssetScanner.hpp"
#include "EditorIcons.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "json.hpp"
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

// ── Navigation ──────────────────────────────────────────────────────────────

void ContentBrowserPanel::navigateTo(const fs::path& dir)
{
    if (dir == m_current_dir) return;

    if (!m_nav_suppress_history)
    {
        // Trim forward history
        if (m_nav_history_index >= 0 && m_nav_history_index < (int)m_nav_history.size() - 1)
            m_nav_history.erase(m_nav_history.begin() + m_nav_history_index + 1, m_nav_history.end());

        m_nav_history.push_back(dir);
        m_nav_history_index = (int)m_nav_history.size() - 1;

        if (m_nav_history.size() > 50)
        {
            m_nav_history.erase(m_nav_history.begin());
            m_nav_history_index--;
        }
    }

    m_current_dir = dir;
}

void ContentBrowserPanel::navigateBack()
{
    if (m_nav_history_index > 0)
    {
        m_nav_suppress_history = true;
        m_nav_history_index--;
        m_current_dir = m_nav_history[m_nav_history_index];
        m_nav_suppress_history = false;
    }
}

void ContentBrowserPanel::navigateForward()
{
    if (m_nav_history_index < (int)m_nav_history.size() - 1)
    {
        m_nav_suppress_history = true;
        m_nav_history_index++;
        m_current_dir = m_nav_history[m_nav_history_index];
        m_nav_suppress_history = false;
    }
}

// ── Directory tree ──────────────────────────────────────────────────────────

void ContentBrowserPanel::drawDirectoryTree(const fs::path& dir)
{
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.is_directory())
            entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    });

    for (const auto& entry : entries)
    {
        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        bool is_selected = (m_current_dir == entry.path());
        if (is_selected)
            node_flags |= ImGuiTreeNodeFlags_Selected;

        bool has_children = false;
        for (const auto& child : fs::directory_iterator(entry.path()))
        {
            if (child.is_directory()) { has_children = true; break; }
        }
        if (!has_children)
            node_flags |= ImGuiTreeNodeFlags_Leaf;

        bool open = ImGui::TreeNodeEx(entry.path().filename().string().c_str(), node_flags);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            navigateTo(entry.path());

        if (open)
        {
            drawDirectoryTree(entry.path());
            ImGui::TreePop();
        }
    }
}

// ── Breadcrumb bar ──────────────────────────────────────────────────────────

void ContentBrowserPanel::drawBreadcrumbBar()
{
    ImVec2 bar_pos = ImGui::GetCursorScreenPos();
    float bar_height = ImGui::GetFrameHeight();
    float bar_width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Background bar
    draw_list->AddRectFilled(
        bar_pos,
        ImVec2(bar_pos.x + bar_width, bar_pos.y + bar_height),
        IM_COL32(20, 20, 20, 255), 2.0f);

    ImGui::SetCursorScreenPos(ImVec2(bar_pos.x + 6.0f, bar_pos.y + 2.0f));

    fs::path relative = fs::relative(m_current_dir, m_base_path.parent_path());
    fs::path accumulated = m_base_path.parent_path();

    bool first = true;
    for (const auto& part : relative)
    {
        if (!first)
        {
            ImGui::SameLine(0, 2.0f);
            ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
            ImGui::SameLine(0, 2.0f);
        }
        first = false;

        accumulated /= part;
        fs::path target = accumulated;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

        if (ImGui::SmallButton(part.string().c_str()))
        {
            if (fs::exists(target) && fs::is_directory(target))
                navigateTo(target);
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // Reserve bar height
    ImGui::SetCursorScreenPos(ImVec2(bar_pos.x, bar_pos.y + bar_height + 2.0f));
}

// ── Status bar (bottom of file area) ────────────────────────────────────────

void ContentBrowserPanel::drawStatusBar()
{
    ImGui::Separator();

    ImGui::Text("%d items", m_visible_item_count);

    // Right-align the thumbnail slider
    float slider_width = 120.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(avail - slider_width + ImGui::GetCursorPosX());
    ImGui::SetNextItemWidth(slider_width);
    ImGui::SliderFloat("##thumb_size", &m_thumbnail_size, 48.0f, 160.0f, "%.0fpx");
}

// ── Icons ───────────────────────────────────────────────────────────────────

void ContentBrowserPanel::drawFileIcon(ImDrawList* draw_list, ImVec2 center, float size, const fs::path& path) const
{
    ImVec4 color4 = getFileColor(path);
    ImU32 color = IM_COL32((int)(color4.x * 255), (int)(color4.y * 255), (int)(color4.z * 255), 255);

    // Determine the FA icon string based on file type
    const char* icon = ICON_FA_FILE;

    if (fs::is_directory(path))
    {
        icon = ICON_FA_FOLDER;
    }
    else
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
            icon = ICON_FA_CUBE;
        else if (ext == ".prefab")
            icon = ICON_FA_PUZZLE_PIECE;
        else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga")
            icon = ICON_FA_FILE_IMAGE;
        else if (path.string().find(".level.json") != std::string::npos)
            icon = ICON_FA_MAP;
        else if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl" || ext == ".slang")
            icon = ICON_FA_FILE_CODE;
    }

    // Render the icon centered using the icon font at the appropriate size
    ImFont* iconFont = ImGuiManager::get().getIconFont();
    if (iconFont)
    {
        float fontSize = size * 0.9f;
        if (fontSize < 14.0f) fontSize = 14.0f;
        if (fontSize > 40.0f) fontSize = 40.0f;
        ImVec2 text_size = iconFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, icon);
        ImVec2 text_pos(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f);
        draw_list->AddText(iconFont, fontSize, text_pos, color, icon);
    }
    else
    {
        // Fallback: render with default font
        ImVec2 text_size = ImGui::CalcTextSize(icon);
        ImVec2 text_pos(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f);
        draw_list->AddText(text_pos, color, icon);
    }
}

// ── Colors ──────────────────────────────────────────────────────────────────

ImVec4 ContentBrowserPanel::getFileColor(const fs::path& path) const
{
    if (fs::is_directory(path)) return ImVec4(1.0f, 0.9f, 0.3f, 1.0f);

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return ImVec4(0.3f, 0.9f, 0.9f, 1.0f);
    if (ext == ".prefab") return ImVec4(0.75f, 0.40f, 1.0f, 1.0f);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
    if (ext == ".json" && path.string().find(".level.json") != std::string::npos) return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
    if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl") return ImVec4(0.7f, 0.4f, 1.0f, 1.0f);
    if (ext == ".bin") return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

// ── Filtering ───────────────────────────────────────────────────────────────

bool ContentBrowserPanel::passesFilter(const fs::path& path) const
{
    if (fs::is_directory(path)) return true;

    // Always hide generated files (.meta, .lodbin)
    if (isGeneratedFile(path)) return false;

    if (m_filter_mode == 0) return true;

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    switch (m_filter_mode)
    {
    case 1: return path.string().find(".level.json") != std::string::npos;
    case 2: return ext == ".gltf" || ext == ".glb" || ext == ".obj";
    case 3: return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
    case 4: return ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl";
    case 5: return ext == ".prefab";
    default: return true;
    }
}

bool ContentBrowserPanel::passesSearchFilter(const fs::path& path) const
{
    if (m_search_buf[0] == '\0') return true;

    std::string filename = path.filename().string();
    std::string search = m_search_buf;

    // Case-insensitive substring match
    std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
    std::transform(search.begin(), search.end(), search.begin(), ::tolower);

    return filename.find(search) != std::string::npos;
}

// ── File actions & helpers ──────────────────────────────────────────────────

void ContentBrowserPanel::handleFileAction(const fs::path& path)
{
    if (fs::is_directory(path))
    {
        navigateTo(path);
    }
    else
    {
        std::string path_str = path.string();

        if (path_str.find(".level.json") != std::string::npos && on_open_level)
        {
            std::replace(path_str.begin(), path_str.end(), '\\', '/');
            on_open_level(path_str);
        }
        else if (isMeshFile(path) && on_open_mesh)
        {
            std::replace(path_str.begin(), path_str.end(), '\\', '/');
            on_open_mesh(path_str);
        }
        else if (isPrefabFile(path) && on_open_prefab)
        {
            std::replace(path_str.begin(), path_str.end(), '\\', '/');
            on_open_prefab(path_str);
        }
    }
}

bool ContentBrowserPanel::isGeneratedFile(const fs::path& path) const
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".lodbin") return true;
    if (ext == ".meta") return true;

    std::string filename = path.filename().string();
    if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".meta")
        return true;

    return false;
}

bool ContentBrowserPanel::isMeshFile(const fs::path& path) const
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool ContentBrowserPanel::isPrefabFile(const fs::path& path) const
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".prefab";
}

bool ContentBrowserPanel::isTextureFile(const fs::path& path) const
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

std::string ContentBrowserPanel::formatFileSize(uintmax_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1048576) return std::to_string(bytes / 1024) + " KB";
    uintmax_t mb_whole = bytes / 1048576;
    uintmax_t mb_frac = (bytes % 1048576) * 10 / 1048576;
    return std::to_string(mb_whole) + "." + std::to_string(mb_frac) + " MB";
}

// ── Metadata display ────────────────────────────────────────────────────────

void ContentBrowserPanel::drawMetadataStatusDot(ImDrawList* draw_list, ImVec2 pos, const fs::path& path) const
{
    if (!isMeshFile(path)) return;

    std::string path_str = path.string();
    std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path_str);

    ImU32 dot_color;
    if (fs::exists(meta_path))
        dot_color = IM_COL32(80, 220, 80, 255);
    else
        dot_color = IM_COL32(220, 180, 40, 255);

    draw_list->AddCircleFilled(pos, 4.0f, dot_color);
}

void ContentBrowserPanel::drawMetadataInfo()
{
    if (!m_has_selected_metadata) return;

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.9f, 1.0f), "Mesh: %s", m_selected_mesh.filename().string().c_str());

    const auto& meta = m_selected_metadata;

    ImGui::Text("Vertices: %zu", meta.vertex_count);
    ImGui::SameLine(0, 16.0f);
    ImGui::Text("Triangles: %zu", meta.triangle_count);

    glm::vec3 size = meta.aabb_max - meta.aabb_min;
    ImGui::Text("AABB: %.1f x %.1f x %.1f", size.x, size.y, size.z);

    if (!meta.lod_levels.empty())
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "LOD Levels: %zu", meta.lod_levels.size());

        if (ImGui::BeginTable("##lod_table", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("LOD");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("Ratio");
            ImGui::TableHeadersRow();

            for (const auto& lod : meta.lod_levels)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("LOD%d", lod.level);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", lod.triangle_count);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", lod.target_ratio * 100.0f);
            }

            ImGui::EndTable();
        }
    }

    if (!meta.generated_at.empty())
    {
        ImGui::TextDisabled("Generated: %s", meta.generated_at.c_str());
    }
}

void ContentBrowserPanel::drawItemContextMenu(const fs::path& path)
{
    if (ImGui::BeginPopupContextItem())
    {
        // --- Common actions for all items ---
        if (ImGui::MenuItem(ICON_FA_PENCIL "  Rename"))
        {
            m_renaming_path = path;
            m_rename_focus_set = false;
            std::string name = path.filename().string();
            std::strncpy(m_rename_buf, name.c_str(), sizeof(m_rename_buf) - 1);
            m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
        }
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete"))
        {
            m_pending_delete = path;
            m_confirm_delete_open = true;
        }

        // --- Directory-specific: create sub-items ---
        if (fs::is_directory(path))
        {
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder"))
                createNewFolder(path);
            if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE "  New Prefab"))
                createEmptyPrefab(path);
            if (ImGui::MenuItem(ICON_FA_MAP "  New Level"))
                createEmptyLevel(path);
        }

        // --- Mesh-specific actions ---
        if (isMeshFile(path))
        {
            ImGui::Separator();
            std::string path_str = path.string();
            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path_str);
            bool has_meta = fs::exists(meta_path);

            if (ImGui::MenuItem(ICON_FA_GEAR "  LOD Settings..."))
            {
                if (on_open_mesh)
                {
                    std::string normalized = path_str;
                    std::replace(normalized.begin(), normalized.end(), '\\', '/');
                    on_open_mesh(normalized);
                }
            }

            if (ImGui::MenuItem(has_meta ? (ICON_FA_ROTATE "  Regenerate LODs") : (ICON_FA_PLUS "  Generate Metadata")))
            {
                if (asset_scanner)
                {
                    std::string normalized = path_str;
                    std::replace(normalized.begin(), normalized.end(), '\\', '/');
                    asset_scanner->regenerateAsset(normalized);
                }
            }
        }

        ImGui::EndPopup();
    }
}

void ContentBrowserPanel::drawBackgroundContextMenu()
{
    if (ImGui::BeginPopupContextWindow("bg_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder"))
            createNewFolder(m_current_dir);
        if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE "  New Prefab"))
            createEmptyPrefab(m_current_dir);
        if (ImGui::MenuItem(ICON_FA_MAP "  New Level"))
            createEmptyLevel(m_current_dir);
        ImGui::EndPopup();
    }
}

void ContentBrowserPanel::createNewFolder(const fs::path& dir)
{
    fs::path folder = dir / "New Folder";
    if (fs::exists(folder))
    {
        for (int i = 1; i < 100; i++)
        {
            folder = dir / ("New Folder (" + std::to_string(i) + ")");
            if (!fs::exists(folder)) break;
        }
    }
    fs::create_directory(folder);
}

void ContentBrowserPanel::createEmptyPrefab(const fs::path& dir)
{
    fs::path file = dir / "New Prefab.prefab";
    if (fs::exists(file))
    {
        for (int i = 1; i < 100; i++)
        {
            file = dir / ("New Prefab (" + std::to_string(i) + ").prefab");
            if (!fs::exists(file)) break;
        }
    }

    nlohmann::json j;
    j["format"] = "garden_prefab";
    j["version"] = 1;
    j["name"] = "New Prefab";
    j["components"]["TagComponent"]["name"] = "New Prefab";
    j["components"]["TransformComponent"]["position"] = {0, 0, 0};
    j["components"]["TransformComponent"]["rotation"] = {0, 0, 0};
    j["components"]["TransformComponent"]["scale"] = {1, 1, 1};

    std::ofstream out(file);
    if (out.is_open())
        out << j.dump(2);
}

void ContentBrowserPanel::createEmptyLevel(const fs::path& dir)
{
    fs::path file = dir / "New Level.level.json";
    if (fs::exists(file))
    {
        for (int i = 1; i < 100; i++)
        {
            file = dir / ("New Level (" + std::to_string(i) + ").level.json");
            if (!fs::exists(file)) break;
        }
    }

    nlohmann::json j;
    j["metadata"]["level_name"] = "New Level";
    j["metadata"]["author"] = "";
    j["metadata"]["version"] = "1.0";
    j["metadata"]["world"]["gravity"] = {{"x", 0.0}, {"y", -1.0}, {"z", 0.0}};
    j["metadata"]["world"]["fixed_delta"] = 0.016;
    j["metadata"]["lighting"]["ambient"] = {{"r", 0.2}, {"g", 0.2}, {"b", 0.2}};
    j["metadata"]["lighting"]["diffuse"] = {{"r", 0.8}, {"g", 0.8}, {"b", 0.8}};
    j["metadata"]["lighting"]["direction"] = {{"x", -0.2}, {"y", -1.0}, {"z", -0.3}};
    j["entities"] = nlohmann::json::array();

    std::ofstream out(file);
    if (out.is_open())
        out << j.dump(2);
}

// ── Asset tooltip ───────────────────────────────────────────────────────────

void ContentBrowserPanel::drawAssetTooltip(const fs::path& path)
{
    // Cache tooltip data (only reload when hovered path changes)
    if (m_tooltip_path != path)
    {
        m_tooltip_path = path;
        m_tooltip_cache = {};
        m_tooltip_cache.loaded = true;

        // File size
        if (fs::exists(path) && fs::is_regular_file(path))
            m_tooltip_cache.file_size_str = formatFileSize(fs::file_size(path));

        // File type
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (isMeshFile(path))
        {
            m_tooltip_cache.file_type = "3D Model (" + ext + ")";
            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path.string());
            Assets::AssetMetadata meta;
            if (Assets::AssetMetadataSerializer::load(meta, meta_path))
            {
                m_tooltip_cache.has_metadata = true;
                m_tooltip_cache.vertex_count = meta.vertex_count;
                m_tooltip_cache.triangle_count = meta.triangle_count;
                m_tooltip_cache.lod_count = meta.lod_levels.size();
            }
        }
        else if (isTextureFile(path))
        {
            m_tooltip_cache.file_type = "Texture (" + ext + ")";
        }
        else if (path.string().find(".level.json") != std::string::npos)
        {
            m_tooltip_cache.file_type = "Level";
        }
        else if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl")
        {
            m_tooltip_cache.file_type = "Shader (" + ext + ")";
        }
        else
        {
            m_tooltip_cache.file_type = ext.empty() ? "File" : ext.substr(1) + " file";
        }
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(300.0f);

    ImGui::TextColored(ImVec4(0.88f, 0.88f, 0.88f, 1.0f), "%s", path.filename().string().c_str());
    ImGui::Separator();
    ImGui::TextDisabled("Type: %s", m_tooltip_cache.file_type.c_str());
    if (!m_tooltip_cache.file_size_str.empty())
        ImGui::TextDisabled("Size: %s", m_tooltip_cache.file_size_str.c_str());

    if (m_tooltip_cache.has_metadata)
    {
        ImGui::Spacing();
        ImGui::Text("Vertices: %zu", m_tooltip_cache.vertex_count);
        ImGui::Text("Triangles: %zu", m_tooltip_cache.triangle_count);
        if (m_tooltip_cache.lod_count > 0)
            ImGui::Text("LOD Levels: %zu", m_tooltip_cache.lod_count);
    }

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ── Main draw ───────────────────────────────────────────────────────────────

void ContentBrowserPanel::draw()
{
    // Seed navigation history on first draw
    if (m_nav_history.empty())
    {
        m_nav_history.push_back(m_current_dir);
        m_nav_history_index = 0;
    }

    ImGui::Begin("Content Browser");

    // === Top toolbar row ===

    // Back/Forward buttons
    {
        bool can_back = m_nav_history_index > 0;
        bool can_fwd = m_nav_history_index < (int)m_nav_history.size() - 1;

        if (!can_back) ImGui::BeginDisabled();
        if (ImGui::SmallButton(ICON_FA_ARROW_LEFT)) navigateBack();
        if (!can_back) ImGui::EndDisabled();
        ImGui::SameLine(0, 2.0f);

        if (!can_fwd) ImGui::BeginDisabled();
        if (ImGui::SmallButton(ICON_FA_ARROW_RIGHT)) navigateForward();
        if (!can_fwd) ImGui::EndDisabled();
        ImGui::SameLine(0, 8.0f);
    }

    // Sources toggle
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (m_show_sources)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.68f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.19f, 0.19f, 1.0f));

        if (ImGui::SmallButton(ICON_FA_FOLDER " Sources"))
            m_show_sources = !m_show_sources;

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 8.0f);
    }

    // Filter dropdown
    ImGui::SetNextItemWidth(100.0f);
    const char* filter_items[] = { "All", "Levels", "Models", "Textures", "Shaders", "Prefabs" };
    ImGui::Combo("##filter", &m_filter_mode, filter_items, 6);
    ImGui::SameLine();

    // Grid/List view toggle
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        bool grid_active = m_grid_view;
        bool list_active = !m_grid_view;

        if (grid_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.68f, 1.0f));
        if (ImGui::SmallButton(ICON_FA_TABLE_CELLS))
            m_grid_view = true;
        if (grid_active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Grid View");

        ImGui::SameLine();

        if (list_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.68f, 1.0f));
        if (ImGui::SmallButton(ICON_FA_LIST))
            m_grid_view = false;
        if (list_active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("List View");

        ImGui::PopStyleVar();
    }

    // === Breadcrumb bar (full width) ===
    drawBreadcrumbBar();

    ImGui::Separator();

    // === Two-pane layout ===
    float tree_width = 180.0f;

    // Left pane: directory tree (collapsible)
    if (m_show_sources)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::BeginChild("DirTree", ImVec2(tree_width, 0), true);
        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_current_dir == m_base_path) root_flags |= ImGuiTreeNodeFlags_Selected;
        bool root_open = ImGui::TreeNodeEx("assets", root_flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            navigateTo(m_base_path);
        if (root_open)
        {
            drawDirectoryTree(m_base_path);
            ImGui::TreePop();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SameLine();
    }

    // Right pane: file area
    ImGui::BeginChild("FileGrid", ImVec2(0, 0), true);

    // Search bar with icon
    ImGui::TextDisabled(ICON_FA_SEARCH);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search assets...", m_search_buf, sizeof(m_search_buf));
    ImGui::Spacing();

    // File area (with room for status bar at bottom)
    float status_bar_height = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("FileArea", ImVec2(0, -status_bar_height - (m_has_selected_metadata ? 140.0f : 0.0f)), false);

    if (fs::exists(m_current_dir) && fs::is_directory(m_current_dir))
    {
        std::vector<fs::directory_entry> dirs, files;
        for (const auto& entry : fs::directory_iterator(m_current_dir))
        {
            if (!passesFilter(entry.path())) continue;
            if (!passesSearchFilter(entry.path())) continue;
            if (entry.is_directory()) dirs.push_back(entry);
            else files.push_back(entry);
        }

        auto sortByName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        };
        std::sort(dirs.begin(), dirs.end(), sortByName);
        std::sort(files.begin(), files.end(), sortByName);

        std::vector<fs::path> all_items;
        if (m_current_dir != m_base_path)
            all_items.push_back(m_current_dir.parent_path());
        for (const auto& d : dirs) all_items.push_back(d.path());
        for (const auto& f : files) all_items.push_back(f.path());

        m_visible_item_count = (int)all_items.size();

        if (m_grid_view)
        {
            float padding = 8.0f;
            float cell_size = m_thumbnail_size + padding;
            float avail_width = ImGui::GetContentRegionAvail().x;
            int columns = std::max(1, (int)(avail_width / cell_size));

            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            if (ImGui::BeginTable("##grid", columns, ImGuiTableFlags_NoBordersInBody))
            {
                for (size_t i = 0; i < all_items.size(); i++)
                {
                    ImGui::TableNextColumn();

                    const fs::path& item = all_items[i];
                    bool is_back = (i == 0 && m_current_dir != m_base_path);
                    std::string filename = is_back ? ".." : item.filename().string();

                    ImGui::PushID((int)i);

                    ImVec2 card_pos = ImGui::GetCursorScreenPos();
                    ImVec2 card_size(m_thumbnail_size, m_thumbnail_size + 20.0f);

                    if (ImGui::InvisibleButton("##card", card_size))
                    {
                        if (!is_back && isMeshFile(item))
                        {
                            m_selected_mesh = item;
                            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(item.string());
                            m_has_selected_metadata = Assets::AssetMetadataSerializer::load(m_selected_metadata, meta_path);

                            if (on_preview_mesh)
                            {
                                std::string p = item.string();
                                std::replace(p.begin(), p.end(), '\\', '/');
                                on_preview_mesh(p);
                            }
                        }
                    }

                    // Drag source for mesh files
                    if (!is_back && isMeshFile(item) && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string path_str = item.string();
                        std::replace(path_str.begin(), path_str.end(), '\\', '/');
                        ImGui::SetDragDropPayload("ASSET_MESH_PATH", path_str.c_str(), path_str.size() + 1);
                        ImVec4 fc = getFileColor(item);
                        ImGui::TextColored(fc, "[M]");
                        ImGui::SameLine();
                        ImGui::Text("%s", item.filename().string().c_str());
                        ImGui::EndDragDropSource();
                    }

                    // Drag source for prefab files
                    if (!is_back && isPrefabFile(item) && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string path_str = item.string();
                        std::replace(path_str.begin(), path_str.end(), '\\', '/');
                        ImGui::SetDragDropPayload("ASSET_PREFAB_PATH", path_str.c_str(), path_str.size() + 1);
                        ImVec4 fc = getFileColor(item);
                        ImGui::TextColored(fc, "[P]");
                        ImGui::SameLine();
                        ImGui::Text("%s", item.filename().string().c_str());
                        ImGui::EndDragDropSource();
                    }

                    bool hovered = ImGui::IsItemHovered();
                    bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(0);

                    // Asset tooltip on hover (with delay)
                    if (!is_back && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        drawAssetTooltip(item);

                    // Right-click context menu
                    if (!is_back)
                        drawItemContextMenu(item);

                    bool is_card_selected = (!is_back && isMeshFile(item) && m_selected_mesh == item);

                    // Card background
                    ImVec2 card_max(card_pos.x + card_size.x, card_pos.y + card_size.y);
                    if (is_card_selected)
                    {
                        draw_list->AddRectFilled(card_pos, card_max, IM_COL32(38, 38, 42, 255), 3.0f);
                    }
                    else
                    {
                        ImU32 bg_col = hovered ? IM_COL32(52, 52, 56, 255) : IM_COL32(30, 30, 33, 255);
                        draw_list->AddRectFilled(card_pos, card_max, bg_col, 3.0f);
                    }

                    // Hover border
                    if (hovered && !is_card_selected)
                        draw_list->AddRect(card_pos, card_max, IM_COL32(60, 60, 65, 180), 3.0f, 0, 1.0f);

                    // Type-colored bottom strip (4px)
                    if (!is_back)
                    {
                        ImVec4 fc = getFileColor(item);
                        ImU32 strip_col = IM_COL32((int)(fc.x * 255), (int)(fc.y * 255), (int)(fc.z * 255), 180);
                        draw_list->AddRectFilled(
                            ImVec2(card_pos.x, card_max.y - 4.0f), card_max, strip_col, 3.0f,
                            ImDrawFlags_RoundCornersBottom);
                    }

                    // Selection glow
                    if (is_card_selected)
                    {
                        draw_list->AddRect(
                            ImVec2(card_pos.x - 1, card_pos.y - 1),
                            ImVec2(card_max.x + 1, card_max.y + 1),
                            IM_COL32(66, 150, 250, 100), 4.0f, 0, 3.0f);
                        draw_list->AddRect(card_pos, card_max, IM_COL32(66, 150, 250, 220), 3.0f, 0, 1.5f);
                    }

                    // Icon
                    ImVec2 icon_center(card_pos.x + m_thumbnail_size * 0.5f,
                                       card_pos.y + m_thumbnail_size * 0.4f);
                    float icon_size = m_thumbnail_size * 0.45f;

                    if (is_back)
                    {
                        ImU32 arrow_col = IM_COL32(255, 230, 77, 255);
                        ImFont* iconFont = ImGuiManager::get().getIconFont();
                        if (iconFont)
                        {
                            float fontSize = icon_size * 0.9f;
                            if (fontSize < 14.0f) fontSize = 14.0f;
                            if (fontSize > 40.0f) fontSize = 40.0f;
                            ImVec2 text_size = iconFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_ARROW_LEFT);
                            ImVec2 text_pos(icon_center.x - text_size.x * 0.5f, icon_center.y - text_size.y * 0.5f);
                            draw_list->AddText(iconFont, fontSize, text_pos, arrow_col, ICON_FA_ARROW_LEFT);
                        }
                    }
                    else
                    {
                        drawFileIcon(draw_list, icon_center, icon_size, item);
                    }

                    // Metadata status dot
                    if (!is_back && isMeshFile(item))
                    {
                        drawMetadataStatusDot(draw_list,
                            ImVec2(card_pos.x + card_size.x - 8.0f, card_pos.y + 8.0f), item);
                    }

                    // Filename with ellipsis truncation (or inline rename)
                    if (!is_back && m_renaming_path == item)
                    {
                        // Inline rename mode
                        float text_y = card_pos.y + m_thumbnail_size - 4.0f;
                        ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 2.0f, text_y));
                        ImGui::SetNextItemWidth(m_thumbnail_size - 4.0f);
                        if (!m_rename_focus_set)
                        {
                            ImGui::SetKeyboardFocusHere();
                            m_rename_focus_set = true;
                        }
                        if (ImGui::InputText("##rename_grid", m_rename_buf, sizeof(m_rename_buf),
                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                        {
                            fs::path new_path = m_renaming_path.parent_path() / m_rename_buf;
                            if (new_path != m_renaming_path && !fs::exists(new_path))
                                fs::rename(m_renaming_path, new_path);
                            m_renaming_path.clear();
                        }
                        if (!ImGui::IsItemActive() && m_rename_focus_set && !m_renaming_path.empty())
                            m_renaming_path.clear();
                    }
                    else
                    {
                        float text_y = card_pos.y + m_thumbnail_size - 4.0f;
                        float text_max_w = m_thumbnail_size - 6.0f;
                        ImVec2 text_pos(card_pos.x + 3.0f, text_y);

                        ImU32 text_col = is_back ? IM_COL32(255, 230, 77, 255) : IM_COL32(210, 210, 210, 255);

                        ImVec2 text_size = ImGui::CalcTextSize(filename.c_str());
                        if (text_size.x > text_max_w)
                        {
                            // Truncate with ellipsis
                            float ellipsis_w = ImGui::CalcTextSize("...").x;
                            float avail = text_max_w - ellipsis_w;
                            int truncate_at = 0;
                            for (int c = 1; c <= (int)filename.size(); c++)
                            {
                                float w = ImGui::CalcTextSize(filename.c_str(), filename.c_str() + c).x;
                                if (w > avail) break;
                                truncate_at = c;
                            }
                            std::string truncated = filename.substr(0, truncate_at) + "...";
                            draw_list->AddText(text_pos, text_col, truncated.c_str());
                        }
                        else
                        {
                            // Center text
                            float offset_x = (text_max_w - text_size.x) * 0.5f;
                            draw_list->AddText(ImVec2(text_pos.x + offset_x, text_y), text_col, filename.c_str());
                        }
                    }

                    if (double_clicked && m_renaming_path.empty())
                    {
                        if (is_back)
                            navigateTo(m_current_dir.parent_path());
                        else
                            handleFileAction(item);
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        else
        {
            // === List view ===
            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            for (size_t i = 0; i < all_items.size(); i++)
            {
                const fs::path& item = all_items[i];
                bool is_back = (i == 0 && m_current_dir != m_base_path);
                std::string filename = is_back ? ".." : item.filename().string();

                ImGui::PushID((int)i);

                ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                float line_h = ImGui::GetTextLineHeight();
                ImVec2 icon_center(icon_pos.x + 8.0f, icon_pos.y + line_h * 0.5f);

                if (is_back)
                {
                    draw_list->AddTriangleFilled(
                        ImVec2(icon_center.x - 4, icon_center.y),
                        ImVec2(icon_center.x + 3, icon_center.y - 4),
                        ImVec2(icon_center.x + 3, icon_center.y + 4),
                        IM_COL32(255, 230, 77, 255));
                }
                else
                {
                    drawFileIcon(draw_list, icon_center, 12.0f, item);
                }

                // Metadata status dot after icon
                if (!is_back && isMeshFile(item))
                {
                    drawMetadataStatusDot(draw_list,
                        ImVec2(icon_center.x + 12.0f, icon_center.y), item);
                }

                ImGui::Dummy(ImVec2(is_back ? 18.0f : (isMeshFile(item) ? 28.0f : 18.0f), line_h));
                ImGui::SameLine();

                ImVec4 color = is_back ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : getFileColor(item);
                ImGui::PushStyleColor(ImGuiCol_Text, color);

                if (!is_back && m_renaming_path == item)
                {
                    // Inline rename mode
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (!m_rename_focus_set)
                    {
                        ImGui::SetKeyboardFocusHere();
                        m_rename_focus_set = true;
                    }
                    if (ImGui::InputText("##rename_list", m_rename_buf, sizeof(m_rename_buf),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                    {
                        fs::path new_path = m_renaming_path.parent_path() / m_rename_buf;
                        if (new_path != m_renaming_path && !fs::exists(new_path))
                            fs::rename(m_renaming_path, new_path);
                        m_renaming_path.clear();
                    }
                    if (!ImGui::IsItemActive() && m_rename_focus_set && !m_renaming_path.empty())
                        m_renaming_path.clear();
                }
                else
                {
                    if (ImGui::Selectable(filename.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        if (!is_back && isMeshFile(item))
                        {
                            m_selected_mesh = item;
                            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(item.string());
                            m_has_selected_metadata = Assets::AssetMetadataSerializer::load(m_selected_metadata, meta_path);

                            if (on_preview_mesh)
                            {
                                std::string p = item.string();
                                std::replace(p.begin(), p.end(), '\\', '/');
                                on_preview_mesh(p);
                            }
                        }

                        if (ImGui::IsMouseDoubleClicked(0) && m_renaming_path.empty())
                        {
                            if (is_back)
                                navigateTo(m_current_dir.parent_path());
                            else
                                handleFileAction(item);
                        }
                    }

                    // Asset tooltip
                    if (!is_back && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        drawAssetTooltip(item);

                    // Drag source for mesh files
                    if (!is_back && isMeshFile(item) && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string path_str = item.string();
                        std::replace(path_str.begin(), path_str.end(), '\\', '/');
                        ImGui::SetDragDropPayload("ASSET_MESH_PATH", path_str.c_str(), path_str.size() + 1);
                        ImVec4 fc = getFileColor(item);
                        ImGui::TextColored(fc, "[M]");
                        ImGui::SameLine();
                        ImGui::Text("%s", item.filename().string().c_str());
                        ImGui::EndDragDropSource();
                    }

                    // Drag source for prefab files
                    if (!is_back && isPrefabFile(item) && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string path_str = item.string();
                        std::replace(path_str.begin(), path_str.end(), '\\', '/');
                        ImGui::SetDragDropPayload("ASSET_PREFAB_PATH", path_str.c_str(), path_str.size() + 1);
                        ImVec4 fc = getFileColor(item);
                        ImGui::TextColored(fc, "[P]");
                        ImGui::SameLine();
                        ImGui::Text("%s", item.filename().string().c_str());
                        ImGui::EndDragDropSource();
                    }
                }

                ImGui::PopStyleColor();

                if (!is_back)
                    drawItemContextMenu(item);

                ImGui::PopID();
            }
        }
    }

    // Background context menu (right-click empty space)
    drawBackgroundContextMenu();

    ImGui::EndChild(); // FileArea

    // Delete confirmation modal
    if (m_confirm_delete_open && !m_pending_delete.empty())
    {
        ImGui::OpenPopup("ConfirmDelete##cb");
        m_confirm_delete_open = false;
    }
    if (ImGui::BeginPopupModal("ConfirmDelete##cb", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete '%s'?", m_pending_delete.filename().string().c_str());
        if (fs::is_directory(m_pending_delete))
            ImGui::TextDisabled("This will delete the folder and all its contents.");
        ImGui::Spacing();

        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            fs::remove_all(m_pending_delete);
            m_pending_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_pending_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Status bar
    drawStatusBar();

    // Metadata info at bottom
    drawMetadataInfo();

    ImGui::EndChild(); // FileGrid

    ImGui::End();
}
