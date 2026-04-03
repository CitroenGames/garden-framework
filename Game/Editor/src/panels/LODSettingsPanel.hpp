#pragma once

#include "Assets/AssetMetadata.hpp"
#include "Assets/AssetMetadataSerializer.hpp"
#include <string>
#include <filesystem>

namespace Assets { class AssetScanner; }

class LODSettingsPanel
{
public:
    Assets::AssetScanner* asset_scanner = nullptr;

    // Open the panel for a given mesh asset
    void open(const std::filesystem::path& mesh_path);
    void close();
    bool isOpen() const { return m_open; }

    void draw();

private:
    bool m_open = false;
    std::filesystem::path m_mesh_path;
    std::string m_mesh_name;

    // Editable metadata loaded from .meta
    Assets::AssetMetadata m_metadata;
    bool m_has_metadata = false;

    // Editable LOD config (working copy — user edits this, then applies)
    struct EditableLODLevel {
        float target_ratio = 0.5f;
        float screen_threshold = 0.3f;
        // Read-only stats (filled after generation)
        size_t triangle_count = 0;
        size_t vertex_count = 0;
    };
    std::vector<EditableLODLevel> m_lod_levels; // LOD1+, LOD0 is implicit (ratio=1.0)
    float m_target_error = 0.01f;
    bool m_lock_borders = false;

    // Status
    bool m_needs_apply = false;
    bool m_is_processing = false;
    std::string m_status_message;

    void loadFromMetadata();
    void applyChanges();
    void addLODLevel();
    void removeLODLevel(int index);
};
