#include "PluginManagerPanel.hpp"
#include "Plugin/EditorPluginHost.hpp"
#include "imgui.h"

static const char* statusText(EditorPluginStatus s)
{
    switch (s)
    {
        case EditorPluginStatus::NotLoaded:       return "Not loaded";
        case EditorPluginStatus::Loaded:          return "Loaded";
        case EditorPluginStatus::Disabled:        return "Disabled";
        case EditorPluginStatus::FailedToLoad:    return "Failed to load";
        case EditorPluginStatus::VersionMismatch: return "Version mismatch";
        case EditorPluginStatus::InitFailed:      return "Init failed";
    }
    return "?";
}

static ImVec4 statusColor(EditorPluginStatus s)
{
    switch (s)
    {
        case EditorPluginStatus::Loaded:          return ImVec4(0.45f, 0.95f, 0.45f, 1.0f);
        case EditorPluginStatus::NotLoaded:       return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
        case EditorPluginStatus::Disabled:        return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
        default:                                  return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
    }
}

void PluginManagerPanel::draw(bool* p_open)
{
    if (!ImGui::Begin("Plugin Manager", p_open))
    {
        ImGui::End();
        return;
    }

    if (!m_host)
    {
        ImGui::TextDisabled("(plugin host not bound)");
        ImGui::End();
        return;
    }

    auto& slots = m_host->slots();
    ImGui::Text("%zu plugin(s) discovered", slots.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan"))
    {
        // Rebuild the slot list from disk (also reloads enabled plugins).
        // Useful after `garden generate-plugin` deploys a new manifest into
        // <engine>/plugins/ while the editor is running.
        m_selected = -1;
        m_host->rescan();
    }
    ImGui::Separator();

    // Two-column layout: list | details
    ImGui::Columns(2, "plugin_mgr_cols", true);
    ImGui::SetColumnWidth(0, 260.0f);

    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto& slot = slots[i];
        ImGui::PushID((int)i);

        ImGui::PushStyleColor(ImGuiCol_Text, statusColor(slot.status));
        bool sel = ((int)i == m_selected);
        const char* name = slot.manifest.name.empty() ? "?" : slot.manifest.name.c_str();
        if (ImGui::Selectable(name, sel))
            m_selected = (int)i;
        ImGui::PopStyleColor();

        ImGui::PopID();
    }

    ImGui::NextColumn();

    if (m_selected >= 0 && m_selected < (int)slots.size())
    {
        auto& slot = slots[m_selected];
        ImGui::Text("%s", slot.manifest.name.c_str());
        if (!slot.manifest.version.empty())
            ImGui::TextDisabled("v%s", slot.manifest.version.c_str());

        ImGui::Separator();

        ImGui::TextColored(statusColor(slot.status), "Status: %s", statusText(slot.status));

        if (!slot.manifest.author.empty())
            ImGui::Text("Author:  %s", slot.manifest.author.c_str());
        if (!slot.manifest.description.empty())
        {
            ImGui::Text("About:");
            ImGui::TextWrapped("%s", slot.manifest.description.c_str());
        }
        ImGui::Text("File:    %s", slot.manifest.file_path.c_str());
        if (!slot.manifest.had_manifest)
            ImGui::TextDisabled("(no .gardenplugin sidecar — metadata derived from filename)");

        if (!slot.manifest.engine_id.empty())
            ImGui::Text("Engine:  %s", slot.manifest.engine_id.c_str());
        if (slot.manifest.min_editor_api > 0)
            ImGui::Text("Min API: %d", slot.manifest.min_editor_api);

        if (!slot.manifest.tags.empty())
        {
            ImGui::Text("Tags:");
            ImGui::SameLine();
            for (size_t t = 0; t < slot.manifest.tags.size(); ++t)
            {
                if (t > 0) ImGui::SameLine();
                // Render each tag as a subtle pill — TextDisabled gives the
                // muted look without needing custom drawing.
                ImGui::TextDisabled("[%s]", slot.manifest.tags[t].c_str());
            }
        }

        if (!slot.last_error.empty())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("Error: %s", slot.last_error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        if (slot.status == EditorPluginStatus::Loaded)
        {
            if (ImGui::Button("Unload"))
                m_host->unloadSlot((size_t)m_selected);
            ImGui::SameLine();
            if (ImGui::Button("Reload"))
                m_host->reloadSlot((size_t)m_selected);
        }
        else
        {
            if (slot.manifest.enabled)
            {
                if (ImGui::Button("Load"))
                    m_host->loadSlot((size_t)m_selected);
            }
            else
            {
                ImGui::TextDisabled("(disabled in manifest — edit plugin.json to enable)");
            }
        }
    }
    else
    {
        ImGui::TextDisabled("(select a plugin on the left)");
    }

    ImGui::Columns(1);
    ImGui::End();
}
