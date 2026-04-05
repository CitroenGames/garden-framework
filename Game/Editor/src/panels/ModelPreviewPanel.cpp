#include "ModelPreviewPanel.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Graphics/renderer.hpp"
#include "Components/Components.hpp"
#include "Assets/LODMeshSerializer.hpp"
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

void ModelPreviewPanel::setPreviewMesh(const std::string& path)
{
    if (path == m_mesh_path && m_mesh) return;

    m_mesh_path = path;
    m_mesh = std::make_shared<mesh>(path, render_api);

    if (m_mesh->is_valid)
    {
        m_mesh->uploadToGPU(render_api);
        m_mesh->computeBounds();

        // Auto-frame the orbit camera
        m_orbit.frameAABB(m_mesh->aabb_min, m_mesh->aabb_max, glm::radians(75.0f));

        // Load metadata
        std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(path);
        m_has_metadata = Assets::AssetMetadataSerializer::load(m_metadata, meta_path);

        // Load LOD levels from .lodbin files
        if (m_has_metadata && m_metadata.lod_enabled)
        {
            std::string mesh_dir = std::filesystem::path(path).parent_path().string();
            if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
                mesh_dir += "/";

            m_mesh->lod_levels.clear();
            for (size_t i = 1; i < m_metadata.lod_levels.size(); ++i)
            {
                const auto& lod_info = m_metadata.lod_levels[i];
                if (lod_info.file_path.empty()) continue;

                std::string lod_path = mesh_dir + lod_info.file_path;
                Assets::LODMeshData lod_data;
                if (Assets::LODMeshSerializer::load(lod_data, lod_path))
                {
                    mesh::LODLevel level;
                    level.screen_threshold = lod_info.screen_threshold;
                    level.vertex_count = lod_data.vertices.size();
                    level.index_count = lod_data.indices.size();
                    level.gpu_mesh = render_api->createMesh();
                    if (level.gpu_mesh)
                    {
                        level.gpu_mesh->uploadIndexedMeshData(
                            lod_data.vertices.data(), lod_data.vertices.size(),
                            lod_data.indices.data(), lod_data.indices.size()
                        );
                    }

                    // Map LOD submesh ranges to original mesh's material textures
                    if (!lod_data.submesh_ranges.empty() && m_mesh->uses_material_ranges)
                    {
                        for (const auto& sr : lod_data.submesh_ranges)
                        {
                            TextureHandle tex = INVALID_TEXTURE;
                            std::string mat_name = "";
                            if (sr.submesh_id < m_mesh->material_ranges.size())
                            {
                                tex = m_mesh->material_ranges[sr.submesh_id].texture;
                                mat_name = m_mesh->material_ranges[sr.submesh_id].material_name;
                            }
                            level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
                        }
                    }

                    m_mesh->lod_levels.push_back(std::move(level));
                }
            }
        }

        m_display_lod = 0;
    }
    else
    {
        m_mesh.reset();
        m_has_metadata = false;
    }

    m_auto_rotate_timer = 0.0f;
}

void ModelPreviewPanel::clearPreview()
{
    m_mesh.reset();
    m_mesh_path.clear();
    m_has_metadata = false;
    m_display_lod = 0;
}

void ModelPreviewPanel::draw()
{
    ImGui::Begin("Asset Preview");

    if (!m_mesh || !render_api)
    {
        ImGui::TextDisabled("Select a mesh in the Content Browser");
        ImGui::End();
        return;
    }

    // Controls toolbar
    drawControls();

    // Get available size for the preview image
    float stats_height = m_show_stats ? 130.0f : 0.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float image_h = avail.y - stats_height;
    if (image_h < 64.0f) image_h = avail.y;

    int new_w = (int)avail.x;
    int new_h = (int)image_h;
    if (new_w > 0 && new_h > 0)
    {
        m_preview_width = new_w;
        m_preview_height = new_h;
    }

    if (m_preview_width > 0 && m_preview_height > 0)
    {
        // Render the preview
        renderPreview();

        // Display the rendered preview
        ImTextureID tex = (ImTextureID)render_api->getPreviewTextureID();
        if (tex)
        {
            ImGui::Image(tex, ImVec2((float)m_preview_width, image_h));

            // Handle orbit camera input when hovering the preview
            if (ImGui::IsItemHovered())
            {
                // Left-drag: orbit
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    m_orbit.orbit(delta.x, delta.y);
                    m_auto_rotate_timer = 0.0f;
                }

                // Scroll: zoom
                float scroll = ImGui::GetIO().MouseWheel;
                if (scroll != 0.0f)
                {
                    m_orbit.zoom(scroll);
                    m_auto_rotate_timer = 0.0f;
                }
            }

            // Auto-rotate after idle
            float dt = ImGui::GetIO().DeltaTime;
            if (m_auto_rotate)
            {
                m_auto_rotate_timer += dt;
                if (m_auto_rotate_timer > 2.0f)
                    m_orbit.yaw += m_orbit.auto_rotate_speed * dt;
            }
        }
    }

    // Stats overlay
    if (m_show_stats)
        drawStatsOverlay();

    ImGui::End();
}

void ModelPreviewPanel::renderPreview()
{
    if (!m_mesh || !render_api || !m_mesh->gpu_mesh) return;

    // Set projection for preview
    float aspect = (float)m_preview_width / (float)m_preview_height;
    // The render API will use its own projection; we set it via setCamera

    // Get the orbit camera
    camera preview_cam = m_orbit.toCamera();

    // Begin preview frame (binds preview render target)
    render_api->beginPreviewFrame(m_preview_width, m_preview_height);

    // Set camera (sets view + projection matrices)
    render_api->setCamera(preview_cam);

    // Override projection for correct aspect ratio
    // Note: setCamera already sets projection, but we need correct aspect for this panel
    // The render API uses the stored FOV and the viewport dimensions to compute projection

    // Set up simple lighting
    render_api->enableLighting(true);
    render_api->setLighting(
        glm::vec3(0.3f, 0.3f, 0.35f),
        glm::vec3(0.9f, 0.85f, 0.8f),
        glm::normalize(glm::vec3(-0.5f, -0.8f, -0.3f))
    );

    // Select LOD
    int prev_force_lod = m_mesh->force_lod;
    m_mesh->force_lod = m_display_lod;

    // Render the mesh at origin
    TransformComponent identity;
    renderer::render_mesh_with_api(*m_mesh, identity, render_api);

    m_mesh->force_lod = prev_force_lod;

    // End preview frame
    render_api->endPreviewFrame();
}

void ModelPreviewPanel::drawControls()
{
    ImGui::Checkbox("Auto-Rotate", &m_auto_rotate);
    ImGui::SameLine();

    // LOD selector
    if (m_mesh && m_mesh->getLODCount() > 1)
    {
        ImGui::SetNextItemWidth(80.0f);
        int max_lod = m_mesh->getLODCount() - 1;
        ImGui::SliderInt("LOD", &m_display_lod, 0, max_lod, "LOD%d");
        ImGui::SameLine();
    }

    ImGui::Checkbox("Stats", &m_show_stats);
    ImGui::SameLine();

    if (ImGui::SmallButton("Frame"))
    {
        m_orbit.frameAABB(m_mesh->aabb_min, m_mesh->aabb_max, glm::radians(75.0f));
        m_auto_rotate_timer = 0.0f;
    }

    ImGui::Separator();
}

void ModelPreviewPanel::drawStatsOverlay()
{
    ImGui::Separator();

    std::string filename = std::filesystem::path(m_mesh_path).filename().string();
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.9f, 1.0f), "%s", filename.c_str());

    // Vertex / triangle counts
    if (m_has_metadata)
    {
        ImGui::Text("Vertices: %zu", m_metadata.vertex_count);
        ImGui::SameLine(0, 16.0f);
        ImGui::Text("Triangles: %zu", m_metadata.triangle_count);
    }
    else
    {
        ImGui::Text("Vertices: %zu", m_mesh->vertices_len);
    }

    // AABB size
    glm::vec3 size = m_mesh->aabb_max - m_mesh->aabb_min;
    ImGui::Text("Size: %.2f x %.2f x %.2f", size.x, size.y, size.z);

    // Materials
    size_t mat_count = m_mesh->uses_material_ranges ? m_mesh->material_ranges.size() : (m_mesh->texture_set ? 1 : 0);
    ImGui::Text("Materials: %zu", mat_count);

    // LOD info
    int lod_count = m_mesh->getLODCount();
    if (lod_count > 1)
    {
        ImGui::Text("LOD Levels: %d", lod_count);
        ImGui::SameLine(0, 16.0f);
        ImGui::Text("Viewing: LOD%d", m_display_lod);
    }

    // File size
    if (m_has_metadata && m_metadata.source_file_size > 0)
    {
        float size_mb = (float)m_metadata.source_file_size / (1024.0f * 1024.0f);
        if (size_mb >= 1.0f)
            ImGui::Text("File: %.1f MB", size_mb);
        else
            ImGui::Text("File: %.0f KB", (float)m_metadata.source_file_size / 1024.0f);
    }
}
