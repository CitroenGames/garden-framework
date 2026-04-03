#include "ContentBrowserPanel.hpp"
#include <algorithm>

namespace fs = std::filesystem;

void ContentBrowserPanel::drawDirectoryTree(const fs::path& dir)
{
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    // Sort entries: directories first, then alphabetical
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

        // Check if has subdirectories
        bool has_children = false;
        for (const auto& child : fs::directory_iterator(entry.path()))
        {
            if (child.is_directory()) { has_children = true; break; }
        }
        if (!has_children)
            node_flags |= ImGuiTreeNodeFlags_Leaf;

        bool open = ImGui::TreeNodeEx(entry.path().filename().string().c_str(), node_flags);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            m_current_dir = entry.path();

        if (open)
        {
            drawDirectoryTree(entry.path());
            ImGui::TreePop();
        }
    }
}

void ContentBrowserPanel::drawBreadcrumbs()
{
    // Build path segments relative to base
    fs::path relative = fs::relative(m_current_dir, m_base_path.parent_path());
    fs::path accumulated = m_base_path.parent_path();

    bool first = true;
    for (const auto& part : relative)
    {
        if (!first)
        {
            ImGui::SameLine(0, 2.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 2.0f);
        }
        first = false;

        accumulated /= part;
        fs::path target = accumulated;

        if (ImGui::SmallButton(part.string().c_str()))
        {
            if (fs::exists(target) && fs::is_directory(target))
                m_current_dir = target;
        }
    }
}

void ContentBrowserPanel::drawFileIcon(ImDrawList* draw_list, ImVec2 center, float size, const fs::path& path) const
{
    ImVec4 color4 = getFileColor(path);
    ImU32 color = IM_COL32((int)(color4.x * 255), (int)(color4.y * 255), (int)(color4.z * 255), 255);
    float hs = size * 0.5f;

    if (fs::is_directory(path))
    {
        // Folder: two overlapping rectangles
        draw_list->AddRectFilled(ImVec2(center.x - hs, center.y - hs * 0.6f),
                                 ImVec2(center.x - hs * 0.2f, center.y - hs * 0.3f), color, 2.0f);
        draw_list->AddRectFilled(ImVec2(center.x - hs, center.y - hs * 0.3f),
                                 ImVec2(center.x + hs, center.y + hs * 0.6f), color, 2.0f);
        return;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
    {
        // Model: cube wireframe
        float s = hs * 0.7f;
        float off = s * 0.35f;
        // Front face
        draw_list->AddRect(ImVec2(center.x - s + off, center.y - s + off),
                           ImVec2(center.x + s * 0.6f + off, center.y + s * 0.6f + off), color, 0, 0, 1.5f);
        // Back face
        draw_list->AddRect(ImVec2(center.x - s - off, center.y - s - off),
                           ImVec2(center.x + s * 0.6f - off, center.y + s * 0.6f - off), color, 0, 0, 1.5f);
        // Connecting lines
        draw_list->AddLine(ImVec2(center.x - s + off, center.y - s + off),
                           ImVec2(center.x - s - off, center.y - s - off), color, 1.5f);
        draw_list->AddLine(ImVec2(center.x + s * 0.6f + off, center.y - s + off),
                           ImVec2(center.x + s * 0.6f - off, center.y - s - off), color, 1.5f);
    }
    else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga")
    {
        // Texture: filled square with diagonal
        draw_list->AddRectFilled(ImVec2(center.x - hs * 0.6f, center.y - hs * 0.6f),
                                 ImVec2(center.x + hs * 0.6f, center.y + hs * 0.6f), color, 2.0f);
        draw_list->AddLine(ImVec2(center.x - hs * 0.6f, center.y + hs * 0.6f),
                           ImVec2(center.x + hs * 0.6f, center.y - hs * 0.6f),
                           IM_COL32(0, 0, 0, 120), 1.5f);
    }
    else if (path.string().find(".level.json") != std::string::npos)
    {
        // Level: 3x3 grid of dots
        for (int gx = -1; gx <= 1; gx++)
            for (int gy = -1; gy <= 1; gy++)
                draw_list->AddCircleFilled(ImVec2(center.x + gx * hs * 0.4f, center.y + gy * hs * 0.4f),
                                           2.5f, color);
    }
    else if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl")
    {
        // Shader: diamond
        draw_list->AddQuadFilled(ImVec2(center.x, center.y - hs * 0.7f),
                                 ImVec2(center.x + hs * 0.6f, center.y),
                                 ImVec2(center.x, center.y + hs * 0.7f),
                                 ImVec2(center.x - hs * 0.6f, center.y), color);
    }
    else
    {
        // Default: plain rectangle outline
        draw_list->AddRect(ImVec2(center.x - hs * 0.4f, center.y - hs * 0.6f),
                           ImVec2(center.x + hs * 0.4f, center.y + hs * 0.6f), color, 2.0f, 0, 1.5f);
    }
}

ImVec4 ContentBrowserPanel::getFileColor(const fs::path& path) const
{
    if (fs::is_directory(path)) return ImVec4(1.0f, 0.9f, 0.3f, 1.0f);  // Yellow

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return ImVec4(0.3f, 0.9f, 0.9f, 1.0f);  // Cyan
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return ImVec4(0.3f, 0.9f, 0.3f, 1.0f);  // Green
    if (ext == ".json" && path.string().find(".level.json") != std::string::npos) return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);  // Orange
    if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl") return ImVec4(0.7f, 0.4f, 1.0f, 1.0f);  // Purple
    if (ext == ".bin") return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // Grey

    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);  // Default grey
}

bool ContentBrowserPanel::passesFilter(const fs::path& path) const
{
    if (m_filter_mode == 0) return true;
    if (fs::is_directory(path)) return true;

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    switch (m_filter_mode)
    {
    case 1: // Levels
        return path.string().find(".level.json") != std::string::npos;
    case 2: // Models
        return ext == ".gltf" || ext == ".glb" || ext == ".obj";
    case 3: // Textures
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
    case 4: // Shaders
        return ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl";
    default:
        return true;
    }
}

void ContentBrowserPanel::handleFileAction(const fs::path& path)
{
    if (fs::is_directory(path))
    {
        m_current_dir = path;
    }
    else
    {
        std::string path_str = path.string();
        if (path_str.find(".level.json") != std::string::npos && on_open_level)
        {
            std::replace(path_str.begin(), path_str.end(), '\\', '/');
            on_open_level(path_str);
        }
    }
}

void ContentBrowserPanel::draw()
{
    ImGui::Begin("Content Browser");

    // --- Header: filter + view toggle + breadcrumbs ---
    ImGui::SetNextItemWidth(120.0f);
    const char* filter_items[] = { "All", "Levels", "Models", "Textures", "Shaders" };
    ImGui::Combo("##filter", &m_filter_mode, filter_items, 5);
    ImGui::SameLine();

    // Grid/List view toggle
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        bool grid_active = m_grid_view;
        bool list_active = !m_grid_view;

        if (grid_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.68f, 1.0f));
        if (ImGui::SmallButton("Grid"))
            m_grid_view = true;
        if (grid_active) ImGui::PopStyleColor();

        ImGui::SameLine();

        if (list_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.68f, 1.0f));
        if (ImGui::SmallButton("List"))
            m_grid_view = false;
        if (list_active) ImGui::PopStyleColor();

        ImGui::PopStyleVar();
    }
    ImGui::SameLine();

    drawBreadcrumbs();

    ImGui::Separator();

    // --- Two-pane layout ---
    float tree_width = 180.0f;

    // Left pane: directory tree
    ImGui::BeginChild("DirTree", ImVec2(tree_width, 0), true);
    ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_current_dir == m_base_path) root_flags |= ImGuiTreeNodeFlags_Selected;
    bool root_open = ImGui::TreeNodeEx("assets", root_flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        m_current_dir = m_base_path;
    if (root_open)
    {
        drawDirectoryTree(m_base_path);
        ImGui::TreePop();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: file grid or list
    ImGui::BeginChild("FileGrid", ImVec2(0, 0), true);

    if (fs::exists(m_current_dir) && fs::is_directory(m_current_dir))
    {
        // Collect and sort entries: directories first, then files
        std::vector<fs::directory_entry> dirs, files;
        for (const auto& entry : fs::directory_iterator(m_current_dir))
        {
            if (!passesFilter(entry.path())) continue;
            if (entry.is_directory()) dirs.push_back(entry);
            else files.push_back(entry);
        }

        auto sortByName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        };
        std::sort(dirs.begin(), dirs.end(), sortByName);
        std::sort(files.begin(), files.end(), sortByName);

        // Combine into one list: back button (if not root) + dirs + files
        std::vector<fs::path> all_items;
        if (m_current_dir != m_base_path)
            all_items.push_back(m_current_dir.parent_path()); // ".." entry
        for (const auto& d : dirs) all_items.push_back(d.path());
        for (const auto& f : files) all_items.push_back(f.path());

        if (m_grid_view)
        {
            // --- Grid/Card view ---
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

                    // Card background
                    ImVec2 card_pos = ImGui::GetCursorScreenPos();
                    ImVec2 card_size(m_thumbnail_size, m_thumbnail_size + 20.0f);

                    // Invisible button for interaction
                    if (ImGui::InvisibleButton("##card", card_size))
                    {
                        // Single click — no action in grid mode (double-click to navigate)
                    }
                    bool hovered = ImGui::IsItemHovered();
                    bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(0);

                    // Draw card background
                    ImU32 bg_col = hovered ? IM_COL32(50, 50, 50, 255) : IM_COL32(35, 35, 35, 255);
                    draw_list->AddRectFilled(card_pos, ImVec2(card_pos.x + card_size.x, card_pos.y + card_size.y),
                                             bg_col, 4.0f);

                    // Draw icon
                    ImVec2 icon_center(card_pos.x + m_thumbnail_size * 0.5f,
                                       card_pos.y + m_thumbnail_size * 0.4f);
                    float icon_size = m_thumbnail_size * 0.45f;

                    if (is_back)
                    {
                        // Back arrow: simple arrow shape
                        ImU32 arrow_col = IM_COL32(255, 230, 77, 255);
                        draw_list->AddTriangleFilled(
                            ImVec2(icon_center.x - icon_size * 0.5f, icon_center.y),
                            ImVec2(icon_center.x + icon_size * 0.2f, icon_center.y - icon_size * 0.4f),
                            ImVec2(icon_center.x + icon_size * 0.2f, icon_center.y + icon_size * 0.4f),
                            arrow_col);
                        draw_list->AddRectFilled(
                            ImVec2(icon_center.x + icon_size * 0.1f, icon_center.y - icon_size * 0.15f),
                            ImVec2(icon_center.x + icon_size * 0.5f, icon_center.y + icon_size * 0.15f),
                            arrow_col);
                    }
                    else
                    {
                        drawFileIcon(draw_list, icon_center, icon_size, item);
                    }

                    // Draw filename (truncated)
                    float text_y = card_pos.y + m_thumbnail_size - 4.0f;
                    float text_max_w = m_thumbnail_size - 4.0f;
                    ImVec2 text_pos(card_pos.x + 2.0f, text_y);

                    draw_list->PushClipRect(text_pos, ImVec2(text_pos.x + text_max_w, text_pos.y + 18.0f), true);
                    ImVec4 text_color4 = is_back ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : getFileColor(item);
                    ImU32 text_col = IM_COL32((int)(text_color4.x * 255), (int)(text_color4.y * 255),
                                              (int)(text_color4.z * 255), 255);
                    draw_list->AddText(text_pos, text_col, filename.c_str());
                    draw_list->PopClipRect();

                    if (double_clicked)
                    {
                        if (is_back)
                            m_current_dir = m_current_dir.parent_path();
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
            // --- List view ---
            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            for (size_t i = 0; i < all_items.size(); i++)
            {
                const fs::path& item = all_items[i];
                bool is_back = (i == 0 && m_current_dir != m_base_path);
                std::string filename = is_back ? ".." : item.filename().string();

                ImGui::PushID((int)i);

                // Small icon via DrawList
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

                ImGui::Dummy(ImVec2(18.0f, line_h));
                ImGui::SameLine();

                ImVec4 color = is_back ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : getFileColor(item);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Selectable(filename.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        if (is_back)
                            m_current_dir = m_current_dir.parent_path();
                        else
                            handleFileAction(item);
                    }
                }
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
        }
    }

    ImGui::EndChild();

    ImGui::End();
}
