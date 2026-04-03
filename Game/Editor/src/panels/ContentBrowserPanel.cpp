#include "ContentBrowserPanel.hpp"
#include "Assets/AssetScanner.hpp"
#include <algorithm>

namespace fs = std::filesystem;

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
        float s = hs * 0.7f;
        float off = s * 0.35f;
        draw_list->AddRect(ImVec2(center.x - s + off, center.y - s + off),
                           ImVec2(center.x + s * 0.6f + off, center.y + s * 0.6f + off), color, 0, 0, 1.5f);
        draw_list->AddRect(ImVec2(center.x - s - off, center.y - s - off),
                           ImVec2(center.x + s * 0.6f - off, center.y + s * 0.6f - off), color, 0, 0, 1.5f);
        draw_list->AddLine(ImVec2(center.x - s + off, center.y - s + off),
                           ImVec2(center.x - s - off, center.y - s - off), color, 1.5f);
        draw_list->AddLine(ImVec2(center.x + s * 0.6f + off, center.y - s + off),
                           ImVec2(center.x + s * 0.6f - off, center.y - s - off), color, 1.5f);
    }
    else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga")
    {
        draw_list->AddRectFilled(ImVec2(center.x - hs * 0.6f, center.y - hs * 0.6f),
                                 ImVec2(center.x + hs * 0.6f, center.y + hs * 0.6f), color, 2.0f);
        draw_list->AddLine(ImVec2(center.x - hs * 0.6f, center.y + hs * 0.6f),
                           ImVec2(center.x + hs * 0.6f, center.y - hs * 0.6f),
                           IM_COL32(0, 0, 0, 120), 1.5f);
    }
    else if (path.string().find(".level.json") != std::string::npos)
    {
        for (int gx = -1; gx <= 1; gx++)
            for (int gy = -1; gy <= 1; gy++)
                draw_list->AddCircleFilled(ImVec2(center.x + gx * hs * 0.4f, center.y + gy * hs * 0.4f),
                                           2.5f, color);
    }
    else if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl")
    {
        draw_list->AddQuadFilled(ImVec2(center.x, center.y - hs * 0.7f),
                                 ImVec2(center.x + hs * 0.6f, center.y),
                                 ImVec2(center.x, center.y + hs * 0.7f),
                                 ImVec2(center.x - hs * 0.6f, center.y), color);
    }
    else
    {
        draw_list->AddRect(ImVec2(center.x - hs * 0.4f, center.y - hs * 0.6f),
                           ImVec2(center.x + hs * 0.4f, center.y + hs * 0.6f), color, 2.0f, 0, 1.5f);
    }
}

ImVec4 ContentBrowserPanel::getFileColor(const fs::path& path) const
{
    if (fs::is_directory(path)) return ImVec4(1.0f, 0.9f, 0.3f, 1.0f);

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return ImVec4(0.3f, 0.9f, 0.9f, 1.0f);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
    if (ext == ".json" && path.string().find(".level.json") != std::string::npos) return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
    if (ext == ".vert" || ext == ".frag" || ext == ".hlsl" || ext == ".metal" || ext == ".spv" || ext == ".glsl") return ImVec4(0.7f, 0.4f, 1.0f, 1.0f);
    if (ext == ".bin") return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

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
    default: return true;
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
        else if (isMeshFile(path) && on_open_mesh)
        {
            std::replace(path_str.begin(), path_str.end(), '\\', '/');
            on_open_mesh(path_str);
        }
    }
}

bool ContentBrowserPanel::isGeneratedFile(const fs::path& path) const
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".lodbin") return true;
    if (ext == ".meta") return true;

    // Also check for .gltf.meta, .obj.meta pattern
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

void ContentBrowserPanel::drawMetadataStatusDot(ImDrawList* draw_list, ImVec2 pos, const fs::path& path) const
{
    if (!isMeshFile(path)) return;

    std::string path_str = path.string();
    std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path_str);

    ImU32 dot_color;
    if (fs::exists(meta_path))
        dot_color = IM_COL32(80, 220, 80, 255);  // Green: meta exists
    else
        dot_color = IM_COL32(220, 180, 40, 255);  // Yellow: no meta

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

    // AABB
    glm::vec3 size = meta.aabb_max - meta.aabb_min;
    ImGui::Text("AABB: %.1f x %.1f x %.1f", size.x, size.y, size.z);

    // LOD levels
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

void ContentBrowserPanel::drawContextMenu(const fs::path& path)
{
    if (!isMeshFile(path)) return;

    if (ImGui::BeginPopupContextItem())
    {
        std::string path_str = path.string();
        std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path_str);
        bool has_meta = fs::exists(meta_path);

        if (ImGui::MenuItem("LOD Settings..."))
        {
            if (on_open_mesh)
            {
                std::string normalized = path_str;
                std::replace(normalized.begin(), normalized.end(), '\\', '/');
                on_open_mesh(normalized);
            }
        }

        if (ImGui::MenuItem(has_meta ? "Regenerate LODs" : "Generate Metadata"))
        {
            if (asset_scanner)
            {
                std::string normalized = path_str;
                std::replace(normalized.begin(), normalized.end(), '\\', '/');
                asset_scanner->regenerateAsset(normalized);
            }
        }

        ImGui::EndPopup();
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

    // Right pane: file grid or list + metadata info
    ImGui::BeginChild("FileGrid", ImVec2(0, 0), true);

    if (fs::exists(m_current_dir) && fs::is_directory(m_current_dir))
    {
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

        std::vector<fs::path> all_items;
        if (m_current_dir != m_base_path)
            all_items.push_back(m_current_dir.parent_path());
        for (const auto& d : dirs) all_items.push_back(d.path());
        for (const auto& f : files) all_items.push_back(f.path());

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
                        // Single click on mesh: select it and show metadata
                        if (!is_back && isMeshFile(item))
                        {
                            m_selected_mesh = item;
                            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(item.string());
                            m_has_selected_metadata = Assets::AssetMetadataSerializer::load(m_selected_metadata, meta_path);
                        }
                    }
                    bool hovered = ImGui::IsItemHovered();
                    bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(0);

                    // Right-click context menu for mesh files
                    if (!is_back)
                        drawContextMenu(item);

                    ImU32 bg_col = hovered ? IM_COL32(50, 50, 50, 255) : IM_COL32(35, 35, 35, 255);
                    draw_list->AddRectFilled(card_pos, ImVec2(card_pos.x + card_size.x, card_pos.y + card_size.y),
                                             bg_col, 4.0f);

                    ImVec2 icon_center(card_pos.x + m_thumbnail_size * 0.5f,
                                       card_pos.y + m_thumbnail_size * 0.4f);
                    float icon_size = m_thumbnail_size * 0.45f;

                    if (is_back)
                    {
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

                    // Metadata status dot (top-right of card) for mesh files
                    if (!is_back && isMeshFile(item))
                    {
                        drawMetadataStatusDot(draw_list,
                            ImVec2(card_pos.x + card_size.x - 8.0f, card_pos.y + 8.0f), item);
                    }

                    // Filename
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

                // Metadata status dot after icon for mesh files
                if (!is_back && isMeshFile(item))
                {
                    drawMetadataStatusDot(draw_list,
                        ImVec2(icon_center.x + 12.0f, icon_center.y), item);
                }

                ImGui::Dummy(ImVec2(is_back ? 18.0f : (isMeshFile(item) ? 28.0f : 18.0f), line_h));
                ImGui::SameLine();

                ImVec4 color = is_back ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : getFileColor(item);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Selectable(filename.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    // Single click on mesh: select it and show metadata
                    if (!is_back && isMeshFile(item))
                    {
                        m_selected_mesh = item;
                        std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(item.string());
                        m_has_selected_metadata = Assets::AssetMetadataSerializer::load(m_selected_metadata, meta_path);
                    }

                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        if (is_back)
                            m_current_dir = m_current_dir.parent_path();
                        else
                            handleFileAction(item);
                    }
                }
                ImGui::PopStyleColor();

                // Right-click context menu
                if (!is_back)
                    drawContextMenu(item);

                ImGui::PopID();
            }
        }
    }

    // Draw metadata info at bottom when a mesh is selected
    drawMetadataInfo();

    ImGui::EndChild();

    ImGui::End();
}
