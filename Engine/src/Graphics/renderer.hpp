#pragma once

#include "Components/camera.hpp"
#include "Components/Components.hpp"
#include "Components/mesh.hpp"
#include "RenderAPI.hpp"
#include "RenderCommandBuffer.hpp"
#include "RenderContext.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "UI/RmlUiManager.h"
#include "Frustum.hpp"
#include "BVH.hpp"
#include "Debug/DebugDraw.hpp"
#include "LODSelector.hpp"
#include "Console/ConVar.hpp"
#include "Threading/FrameSync.hpp"
#include <entt/entt.hpp>
#include <algorithm>
#include <future>
#include <thread>

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
        std::vector<GPUPointLight> points;
        std::vector<GPUSpotLight>  spots;
        points.reserve(64);
        spots.reserve(64);

        auto point_view = registry.view<PointLightComponent, TransformComponent>();
        for (auto entity : point_view)
        {
            const auto& pl = point_view.get<PointLightComponent>(entity);
            const auto& t  = point_view.get<TransformComponent>(entity);
            GPUPointLight gpu{};
            gpu.position    = t.position;
            gpu.range       = pl.range;
            gpu.color       = pl.color;
            gpu.intensity   = pl.intensity;
            gpu.attenuation = glm::vec3(pl.constant_attenuation, pl.linear_attenuation, pl.quadratic_attenuation);
            gpu._pad0       = 0.0f;
            points.push_back(gpu);
        }

        auto spot_view = registry.view<SpotLightComponent, TransformComponent>();
        for (auto entity : spot_view)
        {
            const auto& sl = spot_view.get<SpotLightComponent>(entity);
            const auto& t  = spot_view.get<TransformComponent>(entity);
            GPUSpotLight gpu{};
            gpu.position    = t.position;
            gpu.range       = sl.range;
            glm::mat4 rot   = glm::eulerAngleYXZ(
                glm::radians(t.rotation.y), glm::radians(t.rotation.x), glm::radians(t.rotation.z));
            gpu.direction   = glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            gpu.intensity   = sl.intensity;
            gpu.color       = sl.color;
            gpu.innerCutoff = glm::cos(glm::radians(sl.inner_cone_angle));
            gpu.attenuation = glm::vec3(sl.constant_attenuation, sl.linear_attenuation, sl.quadratic_attenuation);
            gpu.outerCutoff = glm::cos(glm::radians(sl.outer_cone_angle));
            spots.push_back(gpu);
        }

        // Forward path (transparents) still uses the 16+16 cbuffer — fill it with
        // the first MAX_LIGHTS of each. Full list goes to the deferred SBs below.
        LightCBuffer light_buffer{};
        int fwd_points = std::min(static_cast<int>(points.size()), MAX_LIGHTS);
        int fwd_spots  = std::min(static_cast<int>(spots.size()),  MAX_LIGHTS);
        for (int i = 0; i < fwd_points; ++i) light_buffer.pointLights[i] = points[i];
        for (int i = 0; i < fwd_spots;  ++i) light_buffer.spotLights[i]  = spots[i];
        light_buffer.numPointLights = fwd_points;
        light_buffer.numSpotLights  = fwd_spots;
        light_buffer.cameraPos      = cam.getPosition();
        light_buffer._pad2          = 0.0f;
        render_api->setPointAndSpotLights(light_buffer);

        render_api->uploadLightBuffers(points.data(), static_cast<int>(points.size()),
                                        spots.data(),  static_cast<int>(spots.size()));
    }

    // Depth prepass helper: render mesh depth-only with transform (immediate mode, legacy)
    static void render_mesh_depth_only(mesh& m, const TransformComponent& transform, IRenderAPI* api)
    {
        if (!m.visible || !api) return;
        api->pushMatrix();
        api->multiplyMatrix(transform.getTransformMatrix());
        api->renderMeshDepthOnly(m);
        api->popMatrix();
    }

    // ========================================================================
    // Command buffer recording helpers (for multicore rendering path)
    // ========================================================================

    // Ensure a mesh is uploaded to the GPU. Must be called on main thread
    // before recording draw commands (recording is GPU-free).
    static void ensure_mesh_uploaded(mesh& m, IRenderAPI* api)
    {
        if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            m.uploadToGPU(api);
    }

    // Record a single mesh draw into a command buffer.
    // Thread-safe: reads mesh state without mutation.
    static void record_mesh_draw(const mesh& m, const TransformComponent& transform,
                                 RenderCommandBuffer& cmds, bool global_lighting)
    {
        if (!m.visible || !m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;

        glm::mat4 model = transform.getTransformMatrix();
        RenderState base_state = m.getRenderState();

        if (m.uses_material_ranges && !m.material_ranges.empty())
        {
            for (const auto& range : m.material_ranges)
            {
                RenderState range_state = base_state;
                if (range.isAlphaMask()) {
                    range_state.alpha_test = true;
                    range_state.alpha_cutoff = range.alpha_cutoff;
                    range_state.blend_mode = BlendMode::None;
                    range_state.depth_write = true;
                } else if (range.isAlphaBlend()) {
                    range_state.blend_mode = BlendMode::Alpha;
                    range_state.depth_write = false;
                    range_state.alpha_test = false;
                    range_state.alpha_cutoff = 0.0f;
                }
                if (range.double_sided)
                    range_state.cull_mode = CullMode::None;

                PSOKey key = PSOKey::fromRenderState(range_state, global_lighting);
                bool has_tex = range.hasValidTexture();
                cmds.recordDrawRangePBR(m.gpu_mesh, model,
                                     has_tex ? range.texture : INVALID_TEXTURE, has_tex,
                                     key, range.start_vertex, range.vertex_count,
                                     glm::vec3(range.base_color_factor), range_state.alpha_cutoff,
                                     range.metallic_factor, range.roughness_factor, range.emissive_factor);
            }
        }
        else
        {
            PSOKey key = PSOKey::fromRenderState(base_state, global_lighting);
            bool has_tex = (m.texture_set && m.texture != INVALID_TEXTURE);
            cmds.recordDraw(m.gpu_mesh, model,
                            has_tex ? m.texture : INVALID_TEXTURE, has_tex,
                            key, base_state.color);
        }
    }

    // Record a LOD mesh draw into a command buffer.
    // Thread-safe: uses lod_level parameter instead of reading m.current_lod.
    static void record_lod_mesh_draw(const mesh& m, const TransformComponent& transform,
                                     RenderCommandBuffer& cmds, bool global_lighting,
                                     IGPUMesh* lod_gpu_mesh, int lod_level)
    {
        if (!lod_gpu_mesh || !lod_gpu_mesh->isUploaded()) return;

        glm::mat4 model = transform.getTransformMatrix();
        RenderState base_state = m.getRenderState();

        // Check for LOD-specific material ranges using the thread-safe accessor
        const auto* lod_ranges = m.getMaterialRangesForLOD(lod_level);

        if (lod_ranges)
        {
            for (const auto& range : *lod_ranges)
            {
                RenderState range_state = base_state;
                if (range.isAlphaMask()) {
                    range_state.alpha_test = true;
                    range_state.alpha_cutoff = range.alpha_cutoff;
                    range_state.blend_mode = BlendMode::None;
                    range_state.depth_write = true;
                } else if (range.isAlphaBlend()) {
                    range_state.blend_mode = BlendMode::Alpha;
                    range_state.depth_write = false;
                    range_state.alpha_test = false;
                    range_state.alpha_cutoff = 0.0f;
                }
                if (range.double_sided)
                    range_state.cull_mode = CullMode::None;

                PSOKey key = PSOKey::fromRenderState(range_state, global_lighting);
                bool has_tex = range.hasValidTexture();
                cmds.recordDrawRangePBR(lod_gpu_mesh, model,
                                     has_tex ? range.texture : INVALID_TEXTURE, has_tex,
                                     key, range.start_vertex, range.vertex_count,
                                     glm::vec3(range.base_color_factor), range_state.alpha_cutoff,
                                     range.metallic_factor, range.roughness_factor, range.emissive_factor);
            }
        }
        else
        {
            // Fall back to first valid texture from original material ranges
            PSOKey key = PSOKey::fromRenderState(base_state, global_lighting);
            TextureHandle tex = INVALID_TEXTURE;
            bool has_tex = false;
            if (m.uses_material_ranges && !m.material_ranges.empty())
            {
                for (const auto& range : m.material_ranges)
                {
                    if (range.hasValidTexture())
                    {
                        tex = range.texture;
                        has_tex = true;
                        break;
                    }
                }
            }
            else if (m.texture_set && m.texture != INVALID_TEXTURE)
            {
                tex = m.texture;
                has_tex = true;
            }
            cmds.recordDraw(lod_gpu_mesh, model, tex, has_tex, key, base_state.color);
        }
    }

    // Record a mesh at a specific LOD level.
    // Thread-safe: uses getGPUMeshForLOD() instead of selectLOD() + getActiveGPUMesh().
    static void record_mesh_at_lod(const mesh& m, const TransformComponent& transform,
                                   RenderCommandBuffer& cmds, bool global_lighting, int lod_level)
    {
        if (!m.visible) return;

        if (!m.lod_levels.empty() && lod_level > 0)
        {
            IGPUMesh* active = m.getGPUMeshForLOD(lod_level);
            if (active && active != m.gpu_mesh)
            {
                record_lod_mesh_draw(m, transform, cmds, global_lighting, active, lod_level);
                return;
            }
        }
        record_mesh_draw(m, transform, cmds, global_lighting);
    }

    // Record a mesh with automatic LOD selection.
    // Thread-safe: computes LOD locally, never writes to mesh state.
    static void record_mesh_with_lod(const mesh& m, const TransformComponent& transform,
                                     RenderCommandBuffer& cmds, bool global_lighting,
                                     const glm::vec3& camera_pos, const glm::mat4& projection)
    {
        if (!m.visible) return;

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

            IGPUMesh* active = m.getGPUMeshForLOD(lod);
            if (active && active != m.gpu_mesh)
            {
                record_lod_mesh_draw(m, transform, cmds, global_lighting, active, lod);
                return;
            }
        }

        record_mesh_draw(m, transform, cmds, global_lighting);
    }

    // Record a depth-prepass draw command.
    // Thread-safe: reads mesh state without mutation.
    static void record_depth_draw(const mesh& m, const TransformComponent& transform,
                                  RenderCommandBuffer& cmds)
    {
        if (!m.visible || !m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
        glm::mat4 model = transform.getTransformMatrix();

        // Check if any material range needs special depth handling
        bool has_alpha_mask = false;
        bool has_alpha_blend = false;
        if (m.uses_material_ranges) {
            for (const auto& range : m.material_ranges) {
                if (range.isAlphaMask()) has_alpha_mask = true;
                if (range.isAlphaBlend()) has_alpha_blend = true;
            }
        }

        if (m.uses_material_ranges && (has_alpha_mask || has_alpha_blend)) {
            for (const auto& range : m.material_ranges) {
                if (range.isAlphaBlend()) continue;  // Skip blend ranges in depth prepass
                PSOKey key = PSOKey::depthPrepass();
                float alpha_cutoff = 0.0f;
                bool has_tex = false;
                TextureHandle tex = INVALID_TEXTURE;
                if (range.isAlphaMask()) {
                    key.alpha_test = true;
                    key.cull = range.double_sided ? CullMode::None : CullMode::Back;
                    alpha_cutoff = range.alpha_cutoff;
                    has_tex = range.hasValidTexture();
                    tex = has_tex ? range.texture : INVALID_TEXTURE;
                }
                cmds.recordDrawRange(m.gpu_mesh, model, tex, has_tex,
                                     key, range.start_vertex, range.vertex_count,
                                     glm::vec3(1.0f), alpha_cutoff);
            }
        } else {
            PSOKey key = PSOKey::depthPrepass();
            cmds.recordDraw(m.gpu_mesh, model, INVALID_TEXTURE, false, key);
        }
    }

    // Record a depth-prepass draw at a specific LOD.
    // Thread-safe: uses getGPUMeshForLOD() instead of selectLOD().
    static void record_depth_draw_at_lod(const mesh& m, const TransformComponent& transform,
                                         RenderCommandBuffer& cmds, int lod_level)
    {
        if (!m.visible || !m.gpu_mesh) return;
        if (m.transparent) return;  // Don't depth-prepass transparent meshes

        glm::mat4 model = transform.getTransformMatrix();

        // Determine which GPU mesh to use
        IGPUMesh* gpu_mesh = m.gpu_mesh;
        if (lod_level > 0 && !m.lod_levels.empty())
        {
            IGPUMesh* active = m.getGPUMeshForLOD(lod_level);
            if (active && active != m.gpu_mesh && active->isUploaded())
                gpu_mesh = active;
        }
        if (!gpu_mesh->isUploaded()) return;

        // Get material ranges for this LOD level
        const auto* ranges = (lod_level > 0) ? m.getMaterialRangesForLOD(lod_level) : nullptr;
        if (!ranges && m.uses_material_ranges && !m.material_ranges.empty())
            ranges = &m.material_ranges;

        if (ranges)
        {
            bool needs_per_range = false;
            for (const auto& range : *ranges) {
                if (range.isAlphaMask() || range.isAlphaBlend()) { needs_per_range = true; break; }
            }

            if (needs_per_range) {
                for (const auto& range : *ranges) {
                    if (range.isAlphaBlend()) continue;  // Skip blend ranges in depth prepass
                    PSOKey key = PSOKey::depthPrepass();
                    float alpha_cutoff = 0.0f;
                    bool has_tex = false;
                    TextureHandle tex = INVALID_TEXTURE;
                    if (range.isAlphaMask()) {
                        key.alpha_test = true;
                        key.cull = range.double_sided ? CullMode::None : CullMode::Back;
                        alpha_cutoff = range.alpha_cutoff;
                        has_tex = range.hasValidTexture();
                        tex = has_tex ? range.texture : INVALID_TEXTURE;
                    }
                    cmds.recordDrawRange(gpu_mesh, model, tex, has_tex,
                                         key, range.start_vertex, range.vertex_count,
                                         glm::vec3(1.0f), alpha_cutoff);
                }
                return;
            }
        }

        // Simple path: no special materials
        PSOKey key = PSOKey::depthPrepass();
        cmds.recordDraw(gpu_mesh, model, INVALID_TEXTURE, false, key);
    }

    // Record a shadow pass draw command.
    // Thread-safe: computes shadow LOD locally, never writes to mesh state.
    static void record_shadow_draw(const mesh& m, const TransformComponent& transform,
                                   RenderCommandBuffer& cmds, int cascade_index)
    {
        if (!m.visible || !m.casts_shadow) return;
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;

        glm::mat4 model = transform.getTransformMatrix();

        // Determine the GPU mesh (LOD selection for shadows)
        IGPUMesh* gpu_mesh = m.gpu_mesh;
        if (!m.lod_levels.empty())
        {
            int shadow_lod = std::min(cascade_index, static_cast<int>(m.lod_levels.size()));
            IGPUMesh* active = m.getGPUMeshForLOD(shadow_lod);
            if (active && active != m.gpu_mesh && active->isUploaded())
                gpu_mesh = active;
        }
        if (!gpu_mesh->isUploaded()) return;

        // Check if any material range needs special shadow handling
        bool has_alpha_mask = false;
        bool has_alpha_blend = false;
        if (m.uses_material_ranges) {
            for (const auto& range : m.material_ranges) {
                if (range.isAlphaMask()) has_alpha_mask = true;
                if (range.isAlphaBlend()) has_alpha_blend = true;
            }
        }

        if (m.uses_material_ranges && (has_alpha_mask || has_alpha_blend)) {
            for (const auto& range : m.material_ranges) {
                if (range.isAlphaBlend()) continue;  // BLEND materials don't cast shadows
                PSOKey key = PSOKey::shadowPass();
                float alpha_cutoff = 0.0f;
                bool has_tex = false;
                TextureHandle tex = INVALID_TEXTURE;
                if (range.isAlphaMask()) {
                    key.alpha_test = true;
                    alpha_cutoff = range.alpha_cutoff;
                    has_tex = range.hasValidTexture();
                    tex = has_tex ? range.texture : INVALID_TEXTURE;
                }
                cmds.recordDrawRange(gpu_mesh, model, tex, has_tex,
                                     key, range.start_vertex, range.vertex_count,
                                     glm::vec3(1.0f), alpha_cutoff);
            }
        } else {
            PSOKey key = PSOKey::shadowPass();
            cmds.recordDraw(gpu_mesh, model, INVALID_TEXTURE, false, key);
        }
    }

    // Ensure all meshes in an entity list are uploaded to the GPU (main thread pre-pass).
    // This must be called before recording draw commands since recording is GPU-free.
    void ensure_meshes_uploaded(entt::registry& registry, const std::vector<entt::entity>& entities)
    {
        for (auto entity : entities)
        {
            if (!registry.valid(entity)) continue;
            auto* mc = registry.try_get<MeshComponent>(entity);
            if (!mc || !mc->m_mesh) continue;
            ensure_mesh_uploaded(*mc->m_mesh, render_api);

            // Also ensure LOD meshes are uploaded
            for (auto& lod : mc->m_mesh->lod_levels)
            {
                if (lod.gpu_mesh && !lod.gpu_mesh->isUploaded())
                {
                    // LOD meshes are pre-uploaded during level loading, but check anyway
                }
            }
        }
    }

    // ========================================================================
    // Parallel command recording (multicore rendering)
    // ========================================================================

    // Minimum entities per chunk for parallel recording to be worthwhile.
    // Below this threshold, single-threaded recording is faster due to job overhead.
    static constexpr size_t PARALLEL_CHUNK_SIZE = 256;

    // Record opaque entity draws in parallel using std::async.
    // Entities must be pre-uploaded and LOD pre-selected before calling.
    // Returns a merged, sorted command buffer ready for replay.
    RenderCommandBuffer record_opaque_parallel(
        entt::registry& registry,
        const std::vector<entt::entity>& entities,
        const std::vector<int>& lod_levels,
        bool global_lighting)
    {
        // For small entity counts, single-threaded recording is faster
        if (entities.size() < PARALLEL_CHUNK_SIZE)
        {
            RenderCommandBuffer cmds;
            cmds.reserve(entities.size());
            for (size_t i = 0; i < entities.size(); ++i)
            {
                if (!registry.valid(entities[i])) continue;
                auto* mc = registry.try_get<MeshComponent>(entities[i]);
                auto* t = registry.try_get<TransformComponent>(entities[i]);
                if (!mc || !t || !mc->m_mesh || !mc->m_mesh->visible) continue;
                record_mesh_at_lod(*mc->m_mesh, *t, cmds, global_lighting, lod_levels[i]);
            }
            cmds.sort();
            return cmds;
        }

        // Split entities into chunks for parallel recording
        size_t num_chunks = (entities.size() + PARALLEL_CHUNK_SIZE - 1) / PARALLEL_CHUNK_SIZE;
        std::vector<RenderContext> contexts(num_chunks);
        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks);

        FrameSync::get().setPhase(FramePhase::ParallelRecord);

        for (size_t c = 0; c < num_chunks; ++c)
        {
            contexts[c].context_index = static_cast<uint32_t>(c);
            size_t start = c * PARALLEL_CHUNK_SIZE;
            size_t end = std::min(start + PARALLEL_CHUNK_SIZE, entities.size());
            contexts[c].command_buffer.reserve(end - start);

            futures.push_back(std::async(std::launch::async,
                [&, c, start, end]() {
                    for (size_t i = start; i < end; ++i)
                    {
                        if (!registry.valid(entities[i])) continue;
                        auto* mc = registry.try_get<MeshComponent>(entities[i]);
                        auto* t = registry.try_get<TransformComponent>(entities[i]);
                        if (!mc || !t || !mc->m_mesh || !mc->m_mesh->visible) continue;
                        record_mesh_at_lod(*mc->m_mesh, *t,
                                           contexts[c].command_buffer, global_lighting, lod_levels[i]);
                    }
                }));
        }

        // Wait for all recording tasks to complete
        for (auto& f : futures)
            f.get();

        FrameSync::get().setPhase(FramePhase::Replay);

        // Merge all command buffers and sort
        RenderCommandBuffer merged;
        size_t total = 0;
        for (const auto& ctx : contexts)
            total += ctx.command_buffer.size();
        merged.reserve(total);

        for (auto& ctx : contexts)
            merged.append(std::move(ctx.command_buffer));

        merged.sort(); // Sort by PSO+texture to minimize state changes
        return merged;
    }

    // Record shadow draws in parallel for a single cascade.
    RenderCommandBuffer record_shadow_parallel(
        entt::registry& registry,
        const std::vector<entt::entity>& entities,
        int cascade_index)
    {
        if (entities.size() < PARALLEL_CHUNK_SIZE)
        {
            RenderCommandBuffer cmds;
            cmds.reserve(entities.size());
            for (auto entity : entities)
            {
                if (!registry.valid(entity)) continue;
                auto* mc = registry.try_get<MeshComponent>(entity);
                auto* t = registry.try_get<TransformComponent>(entity);
                if (mc && t && mc->m_mesh && mc->m_mesh->visible && mc->m_mesh->casts_shadow)
                    record_shadow_draw(*mc->m_mesh, *t, cmds, cascade_index);
            }
            return cmds;
        }

        size_t num_chunks = (entities.size() + PARALLEL_CHUNK_SIZE - 1) / PARALLEL_CHUNK_SIZE;
        std::vector<RenderCommandBuffer> buffers(num_chunks);
        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks);

        FrameSync::get().setPhase(FramePhase::ParallelRecord);

        for (size_t c = 0; c < num_chunks; ++c)
        {
            size_t start = c * PARALLEL_CHUNK_SIZE;
            size_t end = std::min(start + PARALLEL_CHUNK_SIZE, entities.size());
            buffers[c].reserve(end - start);

            futures.push_back(std::async(std::launch::async,
                [&, c, start, end, cascade_index]() {
                    for (size_t i = start; i < end; ++i)
                    {
                        if (!registry.valid(entities[i])) continue;
                        auto* mc = registry.try_get<MeshComponent>(entities[i]);
                        auto* t = registry.try_get<TransformComponent>(entities[i]);
                        if (mc && t && mc->m_mesh && mc->m_mesh->visible && mc->m_mesh->casts_shadow)
                            record_shadow_draw(*mc->m_mesh, *t, buffers[c], cascade_index);
                    }
                }));
        }

        for (auto& f : futures)
            f.get();

        FrameSync::get().setPhase(FramePhase::Replay);

        RenderCommandBuffer merged;
        size_t total = 0;
        for (const auto& b : buffers) total += b.size();
        merged.reserve(total);
        for (auto& b : buffers) merged.append(std::move(b));
        return merged;
    }

    // Record depth prepass draws in parallel.
    RenderCommandBuffer record_depth_parallel(
        entt::registry& registry,
        const std::vector<entt::entity>& entities,
        const std::vector<int>& lod_levels)
    {
        if (entities.size() < PARALLEL_CHUNK_SIZE)
        {
            RenderCommandBuffer cmds;
            cmds.reserve(entities.size());
            for (size_t i = 0; i < entities.size(); ++i)
            {
                if (!registry.valid(entities[i])) continue;
                auto* mc = registry.try_get<MeshComponent>(entities[i]);
                auto* t = registry.try_get<TransformComponent>(entities[i]);
                if (!mc || !t || !mc->m_mesh || !mc->m_mesh->visible) continue;
                record_depth_draw_at_lod(*mc->m_mesh, *t, cmds, lod_levels[i]);
            }
            return cmds;
        }

        size_t num_chunks = (entities.size() + PARALLEL_CHUNK_SIZE - 1) / PARALLEL_CHUNK_SIZE;
        std::vector<RenderCommandBuffer> buffers(num_chunks);
        std::vector<std::future<void>> futures;
        futures.reserve(num_chunks);

        FrameSync::get().setPhase(FramePhase::ParallelRecord);

        for (size_t c = 0; c < num_chunks; ++c)
        {
            size_t start = c * PARALLEL_CHUNK_SIZE;
            size_t end = std::min(start + PARALLEL_CHUNK_SIZE, entities.size());
            buffers[c].reserve(end - start);

            futures.push_back(std::async(std::launch::async,
                [&, c, start, end]() {
                    for (size_t i = start; i < end; ++i)
                    {
                        if (!registry.valid(entities[i])) continue;
                        auto* mc = registry.try_get<MeshComponent>(entities[i]);
                        auto* t = registry.try_get<TransformComponent>(entities[i]);
                        if (!mc || !t || !mc->m_mesh || !mc->m_mesh->visible) continue;
                        record_depth_draw_at_lod(*mc->m_mesh, *t, buffers[c], lod_levels[i]);
                    }
                }));
        }

        for (auto& f : futures)
            f.get();

        FrameSync::get().setPhase(FramePhase::Replay);

        RenderCommandBuffer merged;
        size_t total = 0;
        for (const auto& b : buffers) total += b.size();
        merged.reserve(total);
        for (auto& b : buffers) merged.append(std::move(b));
        return merged;
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

    // Render a LOD mesh: temporarily swaps gpu_mesh and uses LOD-specific material ranges.
    // WARNING: NOT thread-safe — mutates and restores mesh state. Main thread only.
    // For parallel recording, use the thread-safe record_lod_mesh_draw() instead.
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
        int lod_idx = m.current_lod.load(std::memory_order_relaxed) - 1;
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

        FrameSync::get().setPhase(FramePhase::PreRender);

        // Sync renderer state from CVars
        bvh_enabled = CVAR_BOOL(r_frustumculling);
        depth_prepass_enabled = CVAR_BOOL(r_depthprepass);
        bool sky_enabled = CVAR_BOOL(r_sky);
        bool dynamic_lights_enabled = CVAR_BOOL(r_dynamiclights);
        bool global_lighting = CVAR_BOOL(r_lighting);
        render_api->setFXAAEnabled(CVAR_BOOL(r_fxaa));
        render_api->setSSAOEnabled(CVAR_BOOL(r_ssao));
        render_api->setShadowQuality(CVAR_INT(r_shadowquality));
        render_api->setDeferredEnabled(CVAR_BOOL(r_deferred));
        render_api->enableLighting(global_lighting);

        last_draw_calls = 0;

        auto view = registry.view<MeshComponent, TransformComponent>();

        // Ensure BVH is built before shadow pass (both passes share the same BVH)
        if (bvh_enabled && scene_bvh.needsRebuild())
            scene_bvh.build(registry);

        // 1. Shadow Pass - CSM with per-cascade frustum culling (command buffer path)
        if (render_api->getShadowQuality() > 0)
        {
        render_api->beginShadowPass(light_direction, c);
        const glm::mat4* cascade_matrices = render_api->getLightSpaceMatrices();

        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);

            RenderCommandBuffer shadow_cmds;

            if (bvh_enabled && cascade_matrices)
            {
                Frustum shadow_frustum;
                shadow_frustum.extractFromViewProjection(cascade_matrices[cascade]);

                std::vector<entt::entity> shadow_entities;
                scene_bvh.queryFrustum(shadow_frustum, shadow_entities);

                ensure_meshes_uploaded(registry, shadow_entities);
                shadow_cmds = record_shadow_parallel(registry, shadow_entities, cascade);
            }
            else
            {
                std::vector<entt::entity> all_shadow;
                for (auto entity : view)
                {
                    auto& mesh_comp = view.get<MeshComponent>(entity);
                    if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible && mesh_comp.m_mesh->casts_shadow)
                    {
                        ensure_mesh_uploaded(*mesh_comp.m_mesh, render_api);
                        all_shadow.push_back(entity);
                    }
                }
                shadow_cmds = record_shadow_parallel(registry, all_shadow, cascade);
            }

            render_api->replayCommandBuffer(shadow_cmds);
        }
        render_api->endShadowPass();
        } // shadow quality > 0

        // 2. Main Render Pass
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));
        render_api->setCamera(c);
        render_api->setLighting(ambient_light, diffuse_light, light_direction);
        if (dynamic_lights_enabled)
        {
            gatherAndSetLights(registry, c);
        }
        else
        {
            LightCBuffer empty_lights{};
            empty_lights.cameraPos = c.getPosition();
            render_api->setPointAndSpotLights(empty_lights);
        }

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

            // Ensure all visible meshes are uploaded before recording
            ensure_meshes_uploaded(registry, opaque_entities);
            ensure_meshes_uploaded(registry, transparent_entities);

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

            // Depth prepass: parallel record, then replay
            if (depth_prepass_enabled && !opaque_entities.empty())
            {
                RenderCommandBuffer depth_cmds = record_depth_parallel(
                    registry, opaque_entities, opaque_lod);

                render_api->beginDepthPrepass();
                render_api->replayCommandBuffer(depth_cmds);
                render_api->endDepthPrepass();
            }

            // Main lit pass: opaques - parallel recording, merge, sort, replay
            {
                RenderCommandBuffer opaque_cmds = record_opaque_parallel(
                    registry, opaque_entities, opaque_lod, global_lighting);
                last_draw_calls += opaque_cmds.size();
                if (render_api->isDeferredActive())
                    render_api->submitDeferredOpaqueCommands(opaque_cmds);
                else
                    render_api->replayCommandBufferParallel(opaque_cmds);
            }

            // Main lit pass: transparents (back-to-front, no sort - order matters)
            // Transparent entities must maintain ordering, so record sequentially.
            {
                RenderCommandBuffer transparent_cmds;
                transparent_cmds.reserve(transparent_entities.size());

                for (auto entity : transparent_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                    {
                        record_mesh_with_lod(*mesh_comp->m_mesh, *t, transparent_cmds,
                                             global_lighting, cam_pos, proj);
                    }
                }

                last_draw_calls += transparent_cmds.size();
                if (render_api->isDeferredActive())
                    render_api->submitDeferredTransparentCommands(transparent_cmds);
                else
                    render_api->replayCommandBuffer(transparent_cmds);
            }
        }
        else
        {
            // No BVH - record all entities into command buffer
            last_total_entities = 0;
            last_visible_entities = 0;

            RenderCommandBuffer all_cmds;

            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);

                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    ensure_mesh_uploaded(*mesh_comp.m_mesh, render_api);
                    record_mesh_with_lod(*mesh_comp.m_mesh, t, all_cmds,
                                         global_lighting, cam_pos, proj);
                    last_total_entities++;
                }
            }
            last_visible_entities = last_total_entities;
            last_draw_calls = all_cmds.size();
            render_api->replayCommandBuffer(all_cmds);
        }

        // Render skybox before post-processing (immediate mode - small sequential workload)
        if (sky_enabled)
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

        // Sync renderer state from CVars
        bvh_enabled = CVAR_BOOL(r_frustumculling);
        depth_prepass_enabled = CVAR_BOOL(r_depthprepass);
        bool sky_enabled = CVAR_BOOL(r_sky);
        bool dynamic_lights_enabled = CVAR_BOOL(r_dynamiclights);
        bool global_lighting = CVAR_BOOL(r_lighting);
        render_api->setFXAAEnabled(CVAR_BOOL(r_fxaa));
        render_api->setSSAOEnabled(CVAR_BOOL(r_ssao));
        render_api->setShadowQuality(CVAR_INT(r_shadowquality));
        render_api->setDeferredEnabled(CVAR_BOOL(r_deferred));
        render_api->enableLighting(global_lighting);

        last_draw_calls = 0;

        auto view = registry.view<MeshComponent, TransformComponent>();

        // Ensure BVH is built before shadow pass
        if (bvh_enabled && scene_bvh.needsRebuild())
            scene_bvh.build(registry);

        // 1. Shadow Pass - CSM with per-cascade frustum culling (command buffer path)
        if (render_api->getShadowQuality() > 0)
        {
        render_api->beginShadowPass(light_direction, c);
        const glm::mat4* cascade_matrices = render_api->getLightSpaceMatrices();

        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);

            RenderCommandBuffer shadow_cmds;

            if (bvh_enabled && cascade_matrices)
            {
                Frustum shadow_frustum;
                shadow_frustum.extractFromViewProjection(cascade_matrices[cascade]);

                std::vector<entt::entity> shadow_entities;
                scene_bvh.queryFrustum(shadow_frustum, shadow_entities);

                ensure_meshes_uploaded(registry, shadow_entities);
                shadow_cmds = record_shadow_parallel(registry, shadow_entities, cascade);
            }
            else
            {
                std::vector<entt::entity> all_shadow;
                for (auto entity : view)
                {
                    auto& mesh_comp = view.get<MeshComponent>(entity);
                    if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible && mesh_comp.m_mesh->casts_shadow)
                    {
                        ensure_mesh_uploaded(*mesh_comp.m_mesh, render_api);
                        all_shadow.push_back(entity);
                    }
                }
                shadow_cmds = record_shadow_parallel(registry, all_shadow, cascade);
            }

            render_api->replayCommandBuffer(shadow_cmds);
        }
        render_api->endShadowPass();
        } // shadow quality > 0

        // 2. Main Render Pass to offscreen
        render_api->beginFrame();
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));
        render_api->setCamera(c);
        render_api->setLighting(ambient_light, diffuse_light, light_direction);
        if (dynamic_lights_enabled)
        {
            gatherAndSetLights(registry, c);
        }
        else
        {
            LightCBuffer empty_lights{};
            empty_lights.cameraPos = c.getPosition();
            render_api->setPointAndSpotLights(empty_lights);
        }

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

            // Ensure all visible meshes are uploaded
            ensure_meshes_uploaded(registry, opaque_entities);
            ensure_meshes_uploaded(registry, transparent_entities);

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

            // Depth prepass: parallel record, then replay
            if (depth_prepass_enabled && !opaque_entities.empty())
            {
                RenderCommandBuffer depth_cmds = record_depth_parallel(
                    registry, opaque_entities, opaque_lod);

                render_api->beginDepthPrepass();
                render_api->replayCommandBuffer(depth_cmds);
                render_api->endDepthPrepass();
            }

            // Main lit pass: opaques - parallel recording
            {
                RenderCommandBuffer opaque_cmds = record_opaque_parallel(
                    registry, opaque_entities, opaque_lod, global_lighting);
                last_draw_calls += opaque_cmds.size();
                if (render_api->isDeferredActive())
                    render_api->submitDeferredOpaqueCommands(opaque_cmds);
                else
                    render_api->replayCommandBufferParallel(opaque_cmds);
            }

            // Main lit pass: transparents (back-to-front, sequential)
            {
                RenderCommandBuffer transparent_cmds;
                transparent_cmds.reserve(transparent_entities.size());

                for (auto entity : transparent_entities)
                {
                    if (!registry.valid(entity)) continue;
                    auto* mesh_comp = registry.try_get<MeshComponent>(entity);
                    auto* t = registry.try_get<TransformComponent>(entity);
                    if (mesh_comp && t && mesh_comp->m_mesh && mesh_comp->m_mesh->visible)
                    {
                        record_mesh_with_lod(*mesh_comp->m_mesh, *t, transparent_cmds,
                                             global_lighting, cam_pos, proj);
                    }
                }

                last_draw_calls += transparent_cmds.size();
                if (render_api->isDeferredActive())
                    render_api->submitDeferredTransparentCommands(transparent_cmds);
                else
                    render_api->replayCommandBuffer(transparent_cmds);
            }
        }
        else
        {
            last_total_entities = 0;
            last_visible_entities = 0;

            RenderCommandBuffer all_cmds;

            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);
                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    ensure_mesh_uploaded(*mesh_comp.m_mesh, render_api);
                    record_mesh_with_lod(*mesh_comp.m_mesh, t, all_cmds,
                                         global_lighting, cam_pos, proj);
                    last_total_entities++;
                }
            }
            last_visible_entities = last_total_entities;
            last_draw_calls = all_cmds.size();
            render_api->replayCommandBuffer(all_cmds);
        }

        if (sky_enabled)
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