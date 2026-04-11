#pragma once

#include "PreviewOrbitCamera.hpp"
#include "Assets/AssetMetadata.hpp"
#include "Assets/AssetMetadataSerializer.hpp"
#include "Components/mesh.hpp"
#include "imgui.h"
#include <string>
#include <memory>

class IRenderAPI;

class ModelPreviewPanel
{
public:
    IRenderAPI* render_api = nullptr;

    void draw(bool* p_open = nullptr);

    void setPreviewMesh(const std::string& path);
    void clearPreview();
    bool hasPreview() const { return m_mesh != nullptr; }

private:
    std::shared_ptr<mesh> m_mesh;
    std::string m_mesh_path;
    PreviewOrbitCamera m_orbit;

    Assets::AssetMetadata m_metadata;
    bool m_has_metadata = false;

    int m_preview_width = 0;
    int m_preview_height = 0;
    bool m_auto_rotate = true;
    float m_auto_rotate_timer = 0.0f;
    int m_display_lod = 0;
    bool m_show_stats = true;

    void renderPreview();
    void drawControls();
    void drawStatsOverlay();
};
