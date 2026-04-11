#include "LODSettingsPanel.hpp"
#include "PanelUtils.hpp"
#include "Assets/AssetScanner.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

void LODSettingsPanel::open(const fs::path& mesh_path)
{
    m_mesh_path = mesh_path;
    m_mesh_name = mesh_path.filename().string();
    m_open = true;
    m_needs_apply = false;
    m_is_processing = false;
    m_status_message.clear();

    // Load existing metadata
    std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(mesh_path.string());
    m_has_metadata = Assets::AssetMetadataSerializer::load(m_metadata, meta_path);

    loadFromMetadata();
}

void LODSettingsPanel::close()
{
    m_open = false;
}

void LODSettingsPanel::loadFromMetadata()
{
    m_lod_levels.clear();
    m_lod0_triangle_count = 0;

    if (m_has_metadata)
    {
        m_target_error = m_metadata.lod_config.target_error_threshold;
        m_lock_borders = m_metadata.lod_config.lock_borders;
        m_allow_attribute_collapse = m_metadata.lod_config.allow_attribute_collapse;
        m_prune_disconnected = m_metadata.lod_config.prune_disconnected;
        m_lod0_triangle_count = m_metadata.triangle_count;

        // Skip LOD0 (it's always the original)
        for (size_t i = 1; i < m_metadata.lod_levels.size(); ++i)
        {
            EditableLODLevel level;
            level.target_ratio = m_metadata.lod_levels[i].target_ratio;
            level.screen_threshold = m_metadata.lod_levels[i].screen_threshold;
            level.triangle_count = m_metadata.lod_levels[i].triangle_count;
            level.vertex_count = m_metadata.lod_levels[i].vertex_count;
            m_lod_levels.push_back(level);
        }
    }
    else
    {
        // Default: 3 LOD levels
        m_target_error = 0.05f;
        m_lock_borders = false;
        m_allow_attribute_collapse = false;
        m_prune_disconnected = false;
        m_lod_levels.push_back({0.5f, 0.3f, 0, 0});
        m_lod_levels.push_back({0.25f, 0.15f, 0, 0});
        m_lod_levels.push_back({0.1f, 0.05f, 0, 0});
    }
}

void LODSettingsPanel::addLODLevel()
{
    float prev_ratio = m_lod_levels.empty() ? 1.0f : m_lod_levels.back().target_ratio;
    float prev_threshold = m_lod_levels.empty() ? 0.3f : m_lod_levels.back().screen_threshold;

    EditableLODLevel level;
    level.target_ratio = std::max(0.01f, prev_ratio * 0.5f);
    level.screen_threshold = std::max(0.01f, prev_threshold * 0.5f);
    level.triangle_count = 0;
    level.vertex_count = 0;
    m_lod_levels.push_back(level);
    m_needs_apply = true;
}

void LODSettingsPanel::removeLODLevel(int index)
{
    if (index >= 0 && index < static_cast<int>(m_lod_levels.size()))
    {
        m_lod_levels.erase(m_lod_levels.begin() + index);
        m_needs_apply = true;
    }
}

void LODSettingsPanel::applyChanges()
{
    if (!asset_scanner)
    {
        m_status_message = "Error: No asset scanner available";
        return;
    }

    // Build LODConfig from editable state
    Assets::AssetMetadata::LODConfig config;
    config.max_lod_levels = static_cast<int>(m_lod_levels.size()) + 1; // +1 for LOD0
    config.target_ratios.clear();
    config.target_ratios.push_back(1.0f); // LOD0
    for (const auto& level : m_lod_levels)
        config.target_ratios.push_back(level.target_ratio);
    config.target_error_threshold = m_target_error;
    config.lock_borders = m_lock_borders;
    config.allow_attribute_collapse = m_allow_attribute_collapse;
    config.prune_disconnected = m_prune_disconnected;

    // Collect screen thresholds to write back after generation
    std::vector<float> screen_thresholds;
    screen_thresholds.push_back(0.0f); // LOD0
    for (const auto& level : m_lod_levels)
        screen_thresholds.push_back(level.screen_threshold);

    m_is_processing = true;
    m_status_message = "Generating LODs...";

    // Regenerate with custom config
    std::string path_str = m_mesh_path.string();
    std::replace(path_str.begin(), path_str.end(), '\\', '/');

    bool success = asset_scanner->processAssetWithConfig(path_str, config, screen_thresholds);

    m_is_processing = false;

    if (success)
    {
        m_status_message = "LODs generated successfully";
        m_needs_apply = false;

        // Reload metadata to get updated stats
        std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path_str);
        m_has_metadata = Assets::AssetMetadataSerializer::load(m_metadata, meta_path);
        loadFromMetadata();

        // Hot-reload LODs into live meshes
        if (on_lods_generated)
            on_lods_generated(path_str);
    }
    else
    {
        m_status_message = "Failed to generate LODs";
    }
}

void LODSettingsPanel::draw()
{
    if (!m_open) return;

    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(("LOD Settings - " + m_mesh_name + "###LODSettings").c_str(), &m_open))
    {
        ImGui::End();
        return;
    }
    PanelMaximizeButton();

    // --- Mesh Info Header ---
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.9f, 1.0f), "Mesh: %s", m_mesh_name.c_str());

    if (m_has_metadata)
    {
        ImGui::Text("Vertices: %zu  |  Triangles: %zu", m_metadata.vertex_count, m_metadata.triangle_count);
        glm::vec3 size = m_metadata.aabb_max - m_metadata.aabb_min;
        ImGui::Text("Bounds: %.1f x %.1f x %.1f", size.x, size.y, size.z);
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "No metadata generated yet");
    }

    ImGui::Separator();

    // --- Simplification Quality (promoted from Advanced — most impactful control) ---
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderFloat("Quality", &m_target_error, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic))
        m_needs_apply = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Controls how much the mesh shape can change during simplification.\n"
            "Lower values preserve detail but may not reach the target triangle count.\n"
            "Higher values allow more aggressive reduction.\n\n"
            "Recommended: 0.01 - 0.1 for most meshes.\n"
            "If LODs aren't reaching their target %%, increase this value.");

    if (ImGui::Checkbox("Lock Borders", &m_lock_borders))
        m_needs_apply = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Prevents simplification from collapsing border edges.\nUseful for tiling meshes or modular geometry.");

    if (ImGui::Checkbox("Collapse Across Seams", &m_allow_attribute_collapse))
        m_needs_apply = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Allow simplification to collapse edges across UV seams and hard edges.\n"
            "Enables much more aggressive reduction on complex models.\n"
            "May cause minor UV/normal artifacts at seam boundaries.");

    if (ImGui::Checkbox("Prune Disconnected", &m_prune_disconnected))
        m_needs_apply = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Remove small disconnected parts of the mesh during simplification.\n"
            "Useful for models with floating geometry or debris.");

    ImGui::Spacing();
    ImGui::Separator();

    // --- LOD0 (always present, not editable) ---
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "LOD 0 (Original)");
    ImGui::Indent();
    if (m_has_metadata)
        ImGui::Text("Triangles: %zu  |  Screen Size: Always", m_metadata.triangle_count);
    else
        ImGui::TextDisabled("Generate to see stats");
    ImGui::Unindent();

    ImGui::Spacing();

    // --- Editable LOD Levels ---
    int remove_index = -1;

    for (int i = 0; i < static_cast<int>(m_lod_levels.size()); ++i)
    {
        auto& level = m_lod_levels[i];
        ImGui::PushID(i);

        ImGui::Separator();

        // Header with remove button
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "LOD %d", i + 1);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton("Remove"))
            remove_index = i;

        ImGui::Indent();

        // Target ratio slider (display as percentage)
        ImGui::SetNextItemWidth(200.0f);
        float ratio_pct = level.target_ratio * 100.0f;
        if (ImGui::SliderFloat("Triangle %", &ratio_pct, 1.0f, 99.0f, "%.0f%%"))
        {
            level.target_ratio = std::clamp(ratio_pct / 100.0f, 0.01f, 0.99f);
            m_needs_apply = true;
        }

        // Screen size threshold slider (display as percentage)
        ImGui::SetNextItemWidth(200.0f);
        float threshold_pct = level.screen_threshold * 100.0f;
        if (ImGui::SliderFloat("Screen Size", &threshold_pct, 0.1f, 100.0f, "%.1f%%"))
        {
            level.screen_threshold = std::clamp(threshold_pct / 100.0f, 0.001f, 1.0f);
            m_needs_apply = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(switch when below this)");

        // Stats with achieved vs target comparison
        if (level.triangle_count > 0 && m_lod0_triangle_count > 0)
        {
            float achieved_pct = static_cast<float>(level.triangle_count) / static_cast<float>(m_lod0_triangle_count) * 100.0f;
            float target_pct = level.target_ratio * 100.0f;
            float diff = achieved_pct - target_pct;

            ImGui::Text("Triangles: %zu  |  Vertices: %zu", level.triangle_count, level.vertex_count);

            // Show achieved percentage with color coding
            if (diff > 5.0f)
            {
                // Achieved is significantly higher than target — simplification couldn't reach target
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "Achieved: %.0f%%  (target: %.0f%% - increase Quality to reach target)",
                    achieved_pct, target_pct);
            }
            else
            {
                // Close to target
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                    "Achieved: %.0f%%", achieved_pct);
            }
        }
        else if (level.triangle_count > 0)
        {
            ImGui::TextDisabled("Triangles: %zu  |  Vertices: %zu", level.triangle_count, level.vertex_count);
        }

        ImGui::Unindent();
        ImGui::PopID();
    }

    // Handle deferred removal
    if (remove_index >= 0)
        removeLODLevel(remove_index);

    ImGui::Spacing();
    ImGui::Separator();

    // --- Add LOD button ---
    if (ImGui::Button("+ Add LOD Level"))
        addLODLevel();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Apply button ---
    {
        bool can_apply = !m_is_processing && asset_scanner;

        if (!can_apply)
            ImGui::BeginDisabled();

        ImVec4 apply_color = m_needs_apply
            ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f)
            : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, apply_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(apply_color.x + 0.1f, apply_color.y + 0.1f, apply_color.z + 0.1f, 1.0f));

        if (ImGui::Button("Apply Changes", ImVec2(200, 30)))
            applyChanges();

        ImGui::PopStyleColor(2);

        if (!can_apply)
            ImGui::EndDisabled();

        ImGui::SameLine();

        if (m_needs_apply)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Unsaved changes");
        else if (!m_status_message.empty())
        {
            bool is_error = m_status_message.find("Failed") != std::string::npos;
            ImVec4 color = is_error ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
            ImGui::TextColored(color, "%s", m_status_message.c_str());
        }
    }

    if (m_has_metadata && !m_metadata.generated_at.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Last generated: %s", m_metadata.generated_at.c_str());
    }

    ImGui::End();
}
