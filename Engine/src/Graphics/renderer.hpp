#pragma once

#include "Components/camera.hpp"
#include "Components/Components.hpp"
#include "Components/mesh.hpp"
#include "RenderAPI.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "UI/RmlUiManager.h"
#include "Frustum.hpp"
#include "BVH.hpp"
#include "Debug/DebugDraw.hpp"
#include "LODSelector.hpp"
#include <entt/entt.hpp>
#include <algorithm>

class renderer
{
public:
    IRenderAPI* render_api;

    // Lighting settings
    glm::vec3 ambient_light{0.2f, 0.2f, 0.2f};
    glm::vec3 diffuse_light{0.8f, 0.8f, 0.8f};
    glm::vec3 light_direction{0.0f, -1.0f, 0.0f};

    // BVH for frustum culling
    SceneBVH scene_bvh;
    bool bvh_enabled = true;

    // Culling statistics
    size_t last_total_entities = 0;
    size_t last_visible_entities = 0;
    size_t last_draw_calls = 0;

    bool depth_prepass_enabled = true;

    renderer() : render_api(nullptr) {};
    renderer(IRenderAPI* api) : render_api(api) {};

    void setRenderAPI(IRenderAPI* api) { render_api = api; }
    void setDepthPrepassEnabled(bool enabled) { depth_prepass_enabled = enabled; }
    bool isDepthPrepassEnabled() const { return depth_prepass_enabled; }

    void set_level_lighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
    {
        ambient_light = ambient;
        diffuse_light = diffuse;
        light_direction = direction;
    }

    // Depth prepass helper: render mesh depth-only with transform
    static void render_mesh_depth_only(mesh& m, const TransformComponent& transform, IRenderAPI* api)
    {
        if (!m.visible || !api) return;
        api->pushMatrix();
        api->multiplyMatrix(transform.getTransformMatrix());
        api->renderMeshDepthOnly(m);
        api->popMatrix();
    }

    // Sort entities by texture handle to minimize state changes during rendering
    void sort_entities_by_state(entt::registry& registry, std::vector<entt::entity>& entities)
    {
        std::sort(entities.begin(), entities.end(),
            [&registry](entt::entity a, entt::entity b) {
                auto* ma = registry.try_get<MeshComponent>(a);
                auto* mb = registry.try_get<MeshComponent>(b);
                if (!ma || !ma->m_mesh) return false;
                if (!mb || !mb->m_mesh) return true;

                // Sort by primary texture handle
                TextureHandle tex_a = ma->m_mesh->texture_set ? ma->m_mesh->texture : 0;
                TextureHandle tex_b = mb->m_mesh->texture_set ? mb->m_mesh->texture : 0;
                if (ma->m_mesh->uses_material_ranges && !ma->m_mesh->material_ranges.empty())
                    tex_a = ma->m_mesh->material_ranges[0].texture;
                if (mb->m_mesh->uses_material_ranges && !mb->m_mesh->material_ranges.empty())
                    tex_b = mb->m_mesh->material_ranges[0].texture;

                return tex_a < tex_b;
            });
    }

    static void render_mesh_with_api(mesh& m, const TransformComponent& transform, IRenderAPI* api)
    {
        if (!m.visible || !api) return;

        // Apply object transformation using the complete transform matrix
        api->pushMatrix();

        // Use the complete transformation matrix that includes scale, rotation, and translation
        glm::mat4 transform_mat = transform.getTransformMatrix();
        api->multiplyMatrix(transform_mat);

        // Get render state from mesh
        RenderState state = m.getRenderState();

        // Check if using multi-material mode
        if (m.uses_material_ranges && !m.material_ranges.empty())
        {
            // Render each material range separately
            for (const auto& range : m.material_ranges)
            {
                // Bind the texture for this material range
                if (range.hasValidTexture())
                {
                    api->bindTexture(range.texture);
                }
                else
                {
                    api->unbindTexture();
                }

                // Render this specific range of vertices
                api->renderMeshRange(m, range.start_vertex, range.vertex_count, state);
            }
        }
        else
        {
            // Single texture mode (backward compatibility)
            if (m.texture_set && m.texture != INVALID_TEXTURE)
            {
                api->bindTexture(m.texture);
            }
            else
            {
                api->unbindTexture();
            }

            // Render entire mesh
            api->renderMesh(m, state);
        }

        api->popMatrix();
    };

    // LOD-aware rendering: selects appropriate LOD before drawing
    static void render_mesh_with_lod(mesh& m, const TransformComponent& transform,
                                      IRenderAPI* api, const glm::vec3& camera_pos,
                                      const glm::mat4& projection)
    {
        if (!m.visible || !api) return;

        // LOD selection
        if (!m.lod_levels.empty() && m.bounds_computed)
        {
            int lod_count = m.getLODCount();
            std::vector<float> thresholds(lod_count, 0.0f);
            for (int i = 0; i < static_cast<int>(m.lod_levels.size()); ++i)
                thresholds[i + 1] = m.lod_levels[i].screen_threshold;

            int lod = LODSelector::selectLOD(
                camera_pos, transform.position,
                m.aabb_min, m.aabb_max,
                projection, lod_count, thresholds.data()
            );
            m.selectLOD(lod);

            // Swap gpu_mesh to active LOD for rendering (renderMesh accesses m.gpu_mesh directly)
            IGPUMesh* active = m.getActiveGPUMesh();
            if (active && active != m.gpu_mesh)
            {
                IGPUMesh* original = m.gpu_mesh;
                m.gpu_mesh = active;
                render_mesh_with_api(m, transform, api);
                m.gpu_mesh = original;
                return;
            }
        }

        render_mesh_with_api(m, transform, api);
    }

    void render_scene(entt::registry& registry, camera& c)
    {
        if (!render_api)
        {
            printf("Error: No render API set for renderer\n");
            return;
        }

        last_draw_calls = 0;

        auto view = registry.view<MeshComponent, TransformComponent>();

        // 1. Shadow Pass - CSM (render each cascade)
        // Shadows need all casters, not just visible ones, to ensure shadows from off-screen objects
        render_api->beginShadowPass(light_direction, c);
        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);
            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);

                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    // Render geometry for shadow map
                    render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
                }
            }
        }
        render_api->endShadowPass();

        // 2. Main Render Pass
        // Begin frame
        render_api->beginFrame();

        // Clear with background color
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));

        // Set up camera
        render_api->setCamera(c);

        // Set up lighting from level data
        render_api->setLighting(
            ambient_light,
            diffuse_light,
            light_direction
        );

        // Frustum culling with BVH
        if (bvh_enabled)
        {
            // Rebuild BVH if needed (marks dirty on first frame or when markDirty() called)
            if (scene_bvh.needsRebuild())
            {
                scene_bvh.build(registry);
            }

            // Extract frustum from view-projection matrix
            Frustum frustum;
            glm::mat4 viewProj = render_api->getProjectionMatrix() * render_api->getViewMatrix();
            frustum.extractFromViewProjection(viewProj);

            // Query visible entities
            std::vector<entt::entity> visible_entities;
            scene_bvh.queryFrustum(frustum, visible_entities);

            // Update stats
            last_total_entities = scene_bvh.getTotalEntities();
            last_visible_entities = visible_entities.size();

            // Sort visible entities by texture to minimize state changes
            sort_entities_by_state(registry, visible_entities);

            // Depth prepass: render depth-only for all visible entities
            if (depth_prepass_enabled && !visible_entities.empty())
            {
                render_api->beginDepthPrepass();
                for (auto entity : visible_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                        render_mesh_depth_only(*mesh_comp->m_mesh, *t, render_api);
                }
                render_api->endDepthPrepass();
            }

            // LOD selection data
            glm::mat4 proj = render_api->getProjectionMatrix();
            glm::vec3 cam_pos = c.getPosition();

            // Render only visible entities (main lit pass) with LOD
            for (auto entity : visible_entities)
            {
                if (!registry.valid(entity)) continue;

                auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                auto* t = registry.try_get<TransformComponent>(entity);

                if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                {
                    render_mesh_with_lod(*mesh_comp->m_mesh, *t, render_api, cam_pos, proj);
                    last_draw_calls++;
                }
            }

            // Restore depth state after depth prepass
            if (depth_prepass_enabled)
            {
                // endDepthPrepass set depth-equal + no depth write
                // Restore for skybox and debug rendering
            }
        }
        else
        {
            // No BVH - render all entities (original behavior)
            last_total_entities = 0;
            last_visible_entities = 0;

            glm::mat4 proj = render_api->getProjectionMatrix();
            glm::vec3 cam_pos = c.getPosition();

            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);

                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    render_mesh_with_lod(*mesh_comp.m_mesh, t, render_api, cam_pos, proj);
                    last_total_entities++;
                    last_draw_calls++;
                }
            }
            last_visible_entities = last_total_entities;
        }

        // Render skybox before post-processing
        render_api->renderSkybox();

        // Render debug lines (after scene, before UI)
        DebugDraw::get().render(render_api, c);

        // Render RmlUi (game UI)
        RmlUiManager::get().render();

        // Render ImGui UI (dev tools)
        ImGuiManager::get().render();

        // End frame
        render_api->endFrame();
    };

    // Render scene to offscreen viewport texture (for editor).
    // Does NOT render ImGui or call endFrame. Call endSceneRender() instead.
    void render_scene_to_texture(entt::registry& registry, camera& c)
    {
        if (!render_api)
        {
            printf("Error: No render API set for renderer\n");
            return;
        }

        last_draw_calls = 0;

        auto view = registry.view<MeshComponent, TransformComponent>();

        // 1. Shadow Pass
        render_api->beginShadowPass(light_direction, c);
        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);
            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);
                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                    render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
            }
        }
        render_api->endShadowPass();

        // 2. Main Render Pass to offscreen
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));
        render_api->setCamera(c);
        render_api->setLighting(ambient_light, diffuse_light, light_direction);

        if (bvh_enabled)
        {
            if (scene_bvh.needsRebuild()) scene_bvh.build(registry);
            Frustum frustum;
            glm::mat4 viewProj = render_api->getProjectionMatrix() * render_api->getViewMatrix();
            frustum.extractFromViewProjection(viewProj);

            std::vector<entt::entity> visible_entities;
            scene_bvh.queryFrustum(frustum, visible_entities);
            last_total_entities = scene_bvh.getTotalEntities();
            last_visible_entities = visible_entities.size();

            // Sort by state (editor path)
            sort_entities_by_state(registry, visible_entities);

            // Depth prepass (editor path)
            if (depth_prepass_enabled && !visible_entities.empty())
            {
                render_api->beginDepthPrepass();
                for (auto entity : visible_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                        render_mesh_depth_only(*mesh_comp->m_mesh, *t, render_api);
                }
                render_api->endDepthPrepass();
            }

            glm::mat4 proj = render_api->getProjectionMatrix();
            glm::vec3 cam_pos = c.getPosition();

            for (auto entity : visible_entities)
            {
                if (!registry.valid(entity)) continue;
                auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                auto* t = registry.try_get<TransformComponent>(entity);
                if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                {
                    render_mesh_with_lod(*mesh_comp->m_mesh, *t, render_api, cam_pos, proj);
                    last_draw_calls++;
                }
            }
        }
        else
        {
            last_total_entities = 0;
            last_visible_entities = 0;

            glm::mat4 proj = render_api->getProjectionMatrix();
            glm::vec3 cam_pos = c.getPosition();

            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);
                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    render_mesh_with_lod(*mesh_comp.m_mesh, t, render_api, cam_pos, proj);
                    last_total_entities++;
                    last_draw_calls++;
                }
            }
            last_visible_entities = last_total_entities;
        }

        render_api->renderSkybox();
        DebugDraw::get().render(render_api, c);

        // Render RmlUi (game UI)
        RmlUiManager::get().render();

        // Finalize to viewport texture (NOT screen, NOT ImGui)
        render_api->endSceneRender();
    };

    // Access the scene BVH (for ray picking, etc.)
    SceneBVH& getSceneBVH() { return scene_bvh; }
    const SceneBVH& getSceneBVH() const { return scene_bvh; }

    // Mark BVH as needing rebuild (call when entities are added/removed/moved)
    void markBVHDirty()
    {
        scene_bvh.markDirty();
    }

    // Toggle BVH frustum culling
    void setBVHEnabled(bool enabled)
    {
        bvh_enabled = enabled;
    }

    bool isBVHEnabled() const
    {
        return bvh_enabled;
    }

    // Get culling statistics
    size_t getTotalEntities() const { return last_total_entities; }
    size_t getVisibleEntities() const { return last_visible_entities; }
    size_t getDrawCalls() const { return last_draw_calls; }
};