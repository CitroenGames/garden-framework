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

    void gatherAndSetLights(entt::registry& registry, camera& cam)
    {
        LightCBuffer light_buffer{};
        int point_count = 0;
        int spot_count = 0;

        auto point_view = registry.view<PointLightComponent, TransformComponent>();
        for (auto entity : point_view)
        {
            if (point_count >= MAX_LIGHTS) break;
            const auto& pl = point_view.get<PointLightComponent>(entity);
            const auto& t = point_view.get<TransformComponent>(entity);
            auto& gpu = light_buffer.pointLights[point_count];
            gpu.position = t.position;
            gpu.range = pl.range;
            gpu.color = pl.color;
            gpu.intensity = pl.intensity;
            gpu.attenuation = glm::vec3(pl.constant_attenuation, pl.linear_attenuation, pl.quadratic_attenuation);
            gpu._pad0 = 0.0f;
            point_count++;
        }

        auto spot_view = registry.view<SpotLightComponent, TransformComponent>();
        for (auto entity : spot_view)
        {
            if (spot_count >= MAX_LIGHTS) break;
            const auto& sl = spot_view.get<SpotLightComponent>(entity);
            const auto& t = spot_view.get<TransformComponent>(entity);
            auto& gpu = light_buffer.spotLights[spot_count];
            gpu.position = t.position;
            gpu.range = sl.range;
            // Derive forward direction from rotation euler angles
            glm::mat4 rot = glm::eulerAngleYXZ(
                glm::radians(t.rotation.y), glm::radians(t.rotation.x), glm::radians(t.rotation.z));
            gpu.direction = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            gpu.intensity = sl.intensity;
            gpu.color = sl.color;
            gpu.innerCutoff = glm::cos(glm::radians(sl.inner_cone_angle));
            gpu.attenuation = glm::vec3(sl.constant_attenuation, sl.linear_attenuation, sl.quadratic_attenuation);
            gpu.outerCutoff = glm::cos(glm::radians(sl.outer_cone_angle));
            spot_count++;
        }

        light_buffer.numPointLights = point_count;
        light_buffer.numSpotLights = spot_count;
        light_buffer.cameraPos = cam.getPosition();
        light_buffer._pad2 = 0.0f;

        render_api->setPointAndSpotLights(light_buffer);
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

    // Sort entities by texture handle (primary) and distance (secondary, front-to-back)
    void sort_entities_by_state(entt::registry& registry, std::vector<entt::entity>& entities,
                                const glm::vec3& cam_pos)
    {
        std::sort(entities.begin(), entities.end(),
            [&registry, &cam_pos](entt::entity a, entt::entity b) {
                auto* ma = registry.try_get<MeshComponent>(a);
                auto* mb = registry.try_get<MeshComponent>(b);
                if (!ma || !ma->m_mesh) return false;
                if (!mb || !mb->m_mesh) return true;

                // Primary: sort by texture handle
                TextureHandle tex_a = ma->m_mesh->texture_set ? ma->m_mesh->texture : 0;
                TextureHandle tex_b = mb->m_mesh->texture_set ? mb->m_mesh->texture : 0;
                if (ma->m_mesh->uses_material_ranges && !ma->m_mesh->material_ranges.empty())
                    tex_a = ma->m_mesh->material_ranges[0].texture;
                if (mb->m_mesh->uses_material_ranges && !mb->m_mesh->material_ranges.empty())
                    tex_b = mb->m_mesh->material_ranges[0].texture;

                if (tex_a != tex_b)
                    return tex_a < tex_b;

                // Secondary: front-to-back within same texture group
                auto* ta = registry.try_get<TransformComponent>(a);
                auto* tb = registry.try_get<TransformComponent>(b);
                if (!ta || !tb) return false;
                return glm::dot(cam_pos - ta->position, cam_pos - ta->position) < glm::dot(cam_pos - tb->position, cam_pos - tb->position);
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

    // Render a LOD mesh: temporarily swaps gpu_mesh and uses LOD-specific material ranges
    static void render_lod_mesh(mesh& m, const TransformComponent& transform,
                                 IRenderAPI* api, IGPUMesh* lod_gpu_mesh)
    {
        IGPUMesh* original_gpu = m.gpu_mesh;
        bool original_mat_ranges = m.uses_material_ranges;
        std::vector<MaterialRange> original_ranges;
        TextureHandle original_texture = m.texture;
        bool original_texture_set = m.texture_set;

        m.gpu_mesh = lod_gpu_mesh;

        // Check if this LOD level has per-submesh material ranges
        int lod_idx = m.current_lod - 1;
        bool has_lod_materials = (lod_idx >= 0 && lod_idx < static_cast<int>(m.lod_levels.size())
                                  && !m.lod_levels[lod_idx].material_ranges.empty());

        if (has_lod_materials)
        {
            // Swap in LOD-specific material ranges
            original_ranges = std::move(m.material_ranges);
            m.material_ranges = m.lod_levels[lod_idx].material_ranges;
            m.uses_material_ranges = true;
        }
        else
        {
            // No per-submesh LOD data — fall back to first valid texture
            m.uses_material_ranges = false;
            if (original_mat_ranges && !m.material_ranges.empty())
            {
                for (const auto& range : m.material_ranges)
                {
                    if (range.hasValidTexture())
                    {
                        m.texture = range.texture;
                        m.texture_set = true;
                        break;
                    }
                }
            }
        }

        render_mesh_with_api(m, transform, api);

        m.gpu_mesh = original_gpu;
        if (has_lod_materials)
            m.material_ranges = std::move(original_ranges);
        m.uses_material_ranges = original_mat_ranges;
        m.texture = original_texture;
        m.texture_set = original_texture_set;
    }

    // LOD-aware rendering: selects appropriate LOD before drawing
    static void render_mesh_with_lod(mesh& m, const TransformComponent& transform,
                                      IRenderAPI* api, const glm::vec3& camera_pos,
                                      const glm::mat4& projection)
    {
        if (!m.visible || !api) return;

        // LOD selection
        if (!m.lod_levels.empty() && m.bounds_computed)
        {
            int lod;
            if (m.force_lod >= 0)
            {
                lod = m.force_lod;
            }
            else
            {
                int lod_count = m.getLODCount();
                std::vector<float> thresholds(lod_count, 0.0f);
                for (int i = 0; i < static_cast<int>(m.lod_levels.size()); ++i)
                    thresholds[i + 1] = m.lod_levels[i].screen_threshold;

                lod = LODSelector::selectLOD(
                    camera_pos, transform.position,
                    m.aabb_min, m.aabb_max,
                    projection, lod_count, thresholds.data(),
                    transform.scale
                );
            }
            m.selectLOD(lod);

            IGPUMesh* active = m.getActiveGPUMesh();
            if (active && active != m.gpu_mesh)
            {
                render_lod_mesh(m, transform, api, active);
                return;
            }
        }

        render_mesh_with_api(m, transform, api);
    }

    // Shadow-pass LOD: use coarser LODs for farther cascades
    static void render_mesh_shadow_lod(mesh& m, const TransformComponent& transform,
                                        IRenderAPI* api, int cascade_index)
    {
        if (!m.visible || !m.casts_shadow || !api) return;

        if (!m.lod_levels.empty())
        {
            int shadow_lod = std::min(cascade_index, static_cast<int>(m.lod_levels.size()));
            m.selectLOD(shadow_lod);
            IGPUMesh* active = m.getActiveGPUMesh();
            if (active && active != m.gpu_mesh)
            {
                render_lod_mesh(m, transform, api, active);
                return;
            }
        }
        render_mesh_with_api(m, transform, api);
    }

    // Render mesh at a pre-selected LOD level
    static void render_mesh_at_lod(mesh& m, const TransformComponent& transform,
                                    IRenderAPI* api, int lod_level)
    {
        if (!m.visible || !api) return;

        if (!m.lod_levels.empty() && lod_level > 0)
        {
            m.selectLOD(lod_level);
            IGPUMesh* active = m.getActiveGPUMesh();
            if (active && active != m.gpu_mesh)
            {
                render_lod_mesh(m, transform, api, active);
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

        // Ensure BVH is built before shadow pass (both passes share the same BVH)
        if (bvh_enabled && scene_bvh.needsRebuild())
            scene_bvh.build(registry);

        // 1. Shadow Pass - CSM with per-cascade frustum culling
        render_api->beginShadowPass(light_direction, c);
        const glm::mat4* cascade_matrices = render_api->getLightSpaceMatrices();

        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);

            if (bvh_enabled && cascade_matrices)
            {
                // Extract frustum from this cascade's light-space matrix and cull
                Frustum shadow_frustum;
                shadow_frustum.extractFromViewProjection(cascade_matrices[cascade]);

                std::vector<entt::entity> shadow_entities;
                scene_bvh.queryFrustum(shadow_frustum, shadow_entities);

                for (auto entity : shadow_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible
                        && mesh_comp->m_mesh->casts_shadow)
                    {
                        render_mesh_shadow_lod(*mesh_comp->m_mesh, *t, render_api, cascade);
                    }
                }
            }
            else
            {
                // Fallback: render all (no BVH or no cascade matrices)
                for (auto entity : view)
                {
                    auto& mesh_comp = view.get<MeshComponent>(entity);
                    const auto& t = view.get<TransformComponent>(entity);
                    if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible && mesh_comp.m_mesh->casts_shadow)
                        render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
                }
            }
        }
        render_api->endShadowPass();

        // 2. Main Render Pass
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));
        render_api->setCamera(c);
        render_api->setLighting(ambient_light, diffuse_light, light_direction);
        gatherAndSetLights(registry, c);

        glm::mat4 proj = render_api->getProjectionMatrix();
        glm::vec3 cam_pos = c.getPosition();

        if (bvh_enabled)
        {
            // Extract camera frustum and query visible entities
            Frustum frustum;
            glm::mat4 viewProj = proj * render_api->getViewMatrix();
            frustum.extractFromViewProjection(viewProj);

            std::vector<entt::entity> visible_entities;
            scene_bvh.queryFrustum(frustum, visible_entities);

            last_total_entities = scene_bvh.getTotalEntities();
            last_visible_entities = visible_entities.size();

            // Partition into opaque and transparent
            std::vector<entt::entity> opaque_entities;
            std::vector<entt::entity> transparent_entities;
            opaque_entities.reserve(visible_entities.size());

            for (auto entity : visible_entities)
            {
                auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                if (mesh_comp && mesh_comp->m_mesh && mesh_comp->m_mesh->transparent)
                    transparent_entities.push_back(entity);
                else
                    opaque_entities.push_back(entity);
            }

            // Sort opaques by texture (primary) + distance (secondary)
            sort_entities_by_state(registry, opaque_entities, cam_pos);

            // Sort transparents back-to-front
            std::sort(transparent_entities.begin(), transparent_entities.end(),
                [&registry, &cam_pos](entt::entity a, entt::entity b) {
                    auto* ta = registry.try_get<TransformComponent>(a);
                    auto* tb = registry.try_get<TransformComponent>(b);
                    if (!ta) return false;
                    if (!tb) return true;
                    return glm::dot(cam_pos - ta->position, cam_pos - ta->position) > glm::dot(cam_pos - tb->position, cam_pos - tb->position);
                });

            // Pre-select LOD for opaque entities (coherent between depth prepass and main pass)
            std::vector<int> opaque_lod(opaque_entities.size(), 0);
            for (size_t i = 0; i < opaque_entities.size(); ++i)
            {
                auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                if (!mesh_comp || !t || !mesh_comp->m_mesh) continue;
                mesh& m = *mesh_comp->m_mesh;

                if (!m.lod_levels.empty() && m.bounds_computed)
                {
                    if (m.force_lod >= 0)
                    {
                        opaque_lod[i] = m.force_lod;
                    }
                    else
                    {
                        int lod_count = m.getLODCount();
                        std::vector<float> thresholds(lod_count, 0.0f);
                        for (int j = 0; j < static_cast<int>(m.lod_levels.size()); ++j)
                            thresholds[j + 1] = m.lod_levels[j].screen_threshold;

                        opaque_lod[i] = LODSelector::selectLOD(
                            cam_pos, t->position, m.aabb_min, m.aabb_max,
                            proj, lod_count, thresholds.data(),
                            t->scale
                        );
                    }
                }
            }

            // Depth prepass: opaque entities only, using pre-selected LOD
            if (depth_prepass_enabled && !opaque_entities.empty())
            {
                render_api->beginDepthPrepass();
                for (size_t i = 0; i < opaque_entities.size(); ++i)
                {
                    if (!registry.valid(opaque_entities[i])) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                    auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                    if (!mesh_comp || !t || !mesh_comp->m_mesh || !mesh_comp->m_mesh->visible) continue;

                    mesh& m = *mesh_comp->m_mesh;
                    if (opaque_lod[i] > 0 && !m.lod_levels.empty())
                    {
                        m.selectLOD(opaque_lod[i]);
                        IGPUMesh* active = m.getActiveGPUMesh();
                        if (active && active != m.gpu_mesh)
                        {
                            IGPUMesh* original = m.gpu_mesh;
                            m.gpu_mesh = active;
                            render_mesh_depth_only(m, *t, render_api);
                            m.gpu_mesh = original;
                            continue;
                        }
                    }
                    render_mesh_depth_only(m, *t, render_api);
                }
                render_api->endDepthPrepass();
            }

            // Main lit pass: opaques with pre-selected LOD
            for (size_t i = 0; i < opaque_entities.size(); ++i)
            {
                if (!registry.valid(opaque_entities[i])) continue;
                auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                if (!mesh_comp || !t || !mesh_comp->m_mesh || !mesh_comp->m_mesh->visible) continue;

                render_mesh_at_lod(*mesh_comp->m_mesh, *t, render_api, opaque_lod[i]);
                last_draw_calls++;
            }

            // Main lit pass: transparents (back-to-front, with LOD)
            for (auto entity : transparent_entities)
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
            // No BVH - render all entities (original behavior)
            last_total_entities = 0;
            last_visible_entities = 0;

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

        // Ensure BVH is built before shadow pass
        if (bvh_enabled && scene_bvh.needsRebuild())
            scene_bvh.build(registry);

        // 1. Shadow Pass - CSM with per-cascade frustum culling
        render_api->beginShadowPass(light_direction, c);
        const glm::mat4* cascade_matrices = render_api->getLightSpaceMatrices();

        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);

            if (bvh_enabled && cascade_matrices)
            {
                Frustum shadow_frustum;
                shadow_frustum.extractFromViewProjection(cascade_matrices[cascade]);

                std::vector<entt::entity> shadow_entities;
                scene_bvh.queryFrustum(shadow_frustum, shadow_entities);

                for (auto entity : shadow_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible
                        && mesh_comp->m_mesh->casts_shadow)
                    {
                        render_mesh_shadow_lod(*mesh_comp->m_mesh, *t, render_api, cascade);
                    }
                }
            }
            else
            {
                for (auto entity : view)
                {
                    auto& mesh_comp = view.get<MeshComponent>(entity);
                    const auto& t = view.get<TransformComponent>(entity);
                    if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible && mesh_comp.m_mesh->casts_shadow)
                        render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
                }
            }
        }
        render_api->endShadowPass();

        // 2. Main Render Pass to offscreen
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));
        render_api->setCamera(c);
        render_api->setLighting(ambient_light, diffuse_light, light_direction);
        gatherAndSetLights(registry, c);

        glm::mat4 proj = render_api->getProjectionMatrix();
        glm::vec3 cam_pos = c.getPosition();

        if (bvh_enabled)
        {
            Frustum frustum;
            glm::mat4 viewProj = proj * render_api->getViewMatrix();
            frustum.extractFromViewProjection(viewProj);

            std::vector<entt::entity> visible_entities;
            scene_bvh.queryFrustum(frustum, visible_entities);
            last_total_entities = scene_bvh.getTotalEntities();
            last_visible_entities = visible_entities.size();

            // Partition into opaque and transparent
            std::vector<entt::entity> opaque_entities;
            std::vector<entt::entity> transparent_entities;
            opaque_entities.reserve(visible_entities.size());

            for (auto entity : visible_entities)
            {
                auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                if (mesh_comp && mesh_comp->m_mesh && mesh_comp->m_mesh->transparent)
                    transparent_entities.push_back(entity);
                else
                    opaque_entities.push_back(entity);
            }

            sort_entities_by_state(registry, opaque_entities, cam_pos);

            std::sort(transparent_entities.begin(), transparent_entities.end(),
                [&registry, &cam_pos](entt::entity a, entt::entity b) {
                    auto* ta = registry.try_get<TransformComponent>(a);
                    auto* tb = registry.try_get<TransformComponent>(b);
                    if (!ta) return false;
                    if (!tb) return true;
                    return glm::dot(cam_pos - ta->position, cam_pos - ta->position) > glm::dot(cam_pos - tb->position, cam_pos - tb->position);
                });

            // Pre-select LOD for opaque entities
            std::vector<int> opaque_lod(opaque_entities.size(), 0);
            for (size_t i = 0; i < opaque_entities.size(); ++i)
            {
                auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                if (!mesh_comp || !t || !mesh_comp->m_mesh) continue;
                mesh& m = *mesh_comp->m_mesh;

                if (!m.lod_levels.empty() && m.bounds_computed)
                {
                    if (m.force_lod >= 0)
                    {
                        opaque_lod[i] = m.force_lod;
                    }
                    else
                    {
                        int lod_count = m.getLODCount();
                        std::vector<float> thresholds(lod_count, 0.0f);
                        for (int j = 0; j < static_cast<int>(m.lod_levels.size()); ++j)
                            thresholds[j + 1] = m.lod_levels[j].screen_threshold;

                        opaque_lod[i] = LODSelector::selectLOD(
                            cam_pos, t->position, m.aabb_min, m.aabb_max,
                            proj, lod_count, thresholds.data(),
                            t->scale
                        );
                    }
                }
            }

            // Depth prepass: opaque entities only, using pre-selected LOD
            if (depth_prepass_enabled && !opaque_entities.empty())
            {
                render_api->beginDepthPrepass();
                for (size_t i = 0; i < opaque_entities.size(); ++i)
                {
                    if (!registry.valid(opaque_entities[i])) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                    auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                    if (!mesh_comp || !t || !mesh_comp->m_mesh || !mesh_comp->m_mesh->visible) continue;

                    mesh& m = *mesh_comp->m_mesh;
                    if (opaque_lod[i] > 0 && !m.lod_levels.empty())
                    {
                        m.selectLOD(opaque_lod[i]);
                        IGPUMesh* active = m.getActiveGPUMesh();
                        if (active && active != m.gpu_mesh)
                        {
                            IGPUMesh* original = m.gpu_mesh;
                            m.gpu_mesh = active;
                            render_mesh_depth_only(m, *t, render_api);
                            m.gpu_mesh = original;
                            continue;
                        }
                    }
                    render_mesh_depth_only(m, *t, render_api);
                }
                render_api->endDepthPrepass();
            }

            // Main lit pass: opaques
            for (size_t i = 0; i < opaque_entities.size(); ++i)
            {
                if (!registry.valid(opaque_entities[i])) continue;
                auto* mesh_comp = registry.try_get<MeshComponent>(opaque_entities[i]);
                auto* t = registry.try_get<TransformComponent>(opaque_entities[i]);
                if (!mesh_comp || !t || !mesh_comp->m_mesh || !mesh_comp->m_mesh->visible) continue;

                render_mesh_at_lod(*mesh_comp->m_mesh, *t, render_api, opaque_lod[i]);
                last_draw_calls++;
            }

            // Main lit pass: transparents (back-to-front)
            for (auto entity : transparent_entities)
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