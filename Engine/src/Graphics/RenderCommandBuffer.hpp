#pragma once

#include "RenderAPI.hpp"
#include "IGPUMesh.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>

// Compact key encoding the pipeline state variant needed for a draw call.
// Used by backends to select the correct PSO (D3D12) or Pipeline (Vulkan).
struct PSOKey
{
    BlendMode blend = BlendMode::None;
    CullMode  cull  = CullMode::Back;
    bool lighting   = true;
    bool depth_only = false;  // depth prepass
    bool shadow     = false;  // shadow pass
    bool alpha_test = false;  // alpha mask (shader discard)

    bool operator==(const PSOKey& o) const
    {
        return blend == o.blend && cull == o.cull &&
               lighting == o.lighting && depth_only == o.depth_only &&
               shadow == o.shadow && alpha_test == o.alpha_test;
    }

    bool operator!=(const PSOKey& o) const { return !(*this == o); }

    // Sortable: group by PSO variant to minimize state changes
    bool operator<(const PSOKey& o) const
    {
        if (shadow != o.shadow) return shadow < o.shadow;
        if (depth_only != o.depth_only) return depth_only < o.depth_only;
        if (lighting != o.lighting) return lighting < o.lighting;
        if (alpha_test != o.alpha_test) return alpha_test < o.alpha_test;
        if (blend != o.blend) return blend < o.blend;
        return cull < o.cull;
    }

    // Build from a RenderState + global lighting flag
    static PSOKey fromRenderState(const RenderState& state, bool global_lighting)
    {
        PSOKey key;
        key.blend = state.blend_mode;
        key.cull = state.cull_mode;
        key.lighting = state.lighting && global_lighting;
        key.alpha_test = state.alpha_test;
        key.depth_only = false;
        key.shadow = false;
        return key;
    }

    static PSOKey depthPrepass()
    {
        PSOKey key;
        key.depth_only = true;
        key.shadow = false;
        key.lighting = false;
        key.blend = BlendMode::None;
        key.cull = CullMode::Back;
        return key;
    }

    static PSOKey shadowPass()
    {
        PSOKey key;
        key.shadow = true;
        key.depth_only = false;
        key.lighting = false;
        key.blend = BlendMode::None;
        key.cull = CullMode::Back;
        return key;
    }
};

// A self-contained draw command. All state needed for rendering is captured
// at record time -- no matrix stack, no "current texture" state.
// This allows the command to be recorded from any thread.
struct DrawCommand
{
    IGPUMesh*     gpu_mesh     = nullptr;
    glm::mat4     model_matrix = glm::mat4(1.0f);
    glm::mat4     normal_matrix = glm::mat4(1.0f);
    TextureHandle texture      = INVALID_TEXTURE;
    bool          use_texture  = false;
    PSOKey        pso_key;
    glm::vec3     color        = glm::vec3(1.0f);
    float         alpha_cutoff = 0.0f;  // >0 triggers alpha test discard in shader
    TextureHandle heightmap_texture = INVALID_TEXTURE;
    bool          use_heightmap_displacement = false;
    float         heightmap_height_scale = 1.0f;
    float         heightmap_height_offset = 0.0f;
    glm::vec2     heightmap_texel_size = glm::vec2(0.0f);

    // PBR material properties
    float         metallic     = 0.0f;
    float         roughness    = 0.5f;
    glm::vec3     emissive     = glm::vec3(0.0f);
    uint32_t      material_flags = 0;

    // For range draws (renderMeshRange). If vertex_count == 0, draw entire mesh.
    size_t start_vertex = 0;
    size_t vertex_count = 0;
};

// CPU-side command buffer that stores a flat list of self-contained draw commands.
// Multiple threads can each own their own RenderCommandBuffer and record independently.
// No GPU calls happen during recording -- all GPU work is deferred to replay.
class RenderCommandBuffer
{
public:
    RenderCommandBuffer() = default;

    // Reserve space for expected number of commands
    void reserve(size_t count) { m_commands.reserve(count); }

    // Record a full-mesh draw command
    void recordDraw(IGPUMesh* gpu_mesh, const glm::mat4& model_matrix,
                    TextureHandle texture, bool use_texture,
                    const PSOKey& pso_key, const glm::vec3& color = glm::vec3(1.0f),
                    TextureHandle heightmap_texture = INVALID_TEXTURE,
                    bool use_heightmap_displacement = false,
                    float heightmap_height_scale = 1.0f,
                    float heightmap_height_offset = 0.0f,
                    glm::vec2 heightmap_texel_size = glm::vec2(0.0f))
    {
        DrawCommand cmd;
        cmd.gpu_mesh = gpu_mesh;
        cmd.model_matrix = model_matrix;
        cmd.normal_matrix = computeNormalMatrix(model_matrix);
        cmd.texture = texture;
        cmd.use_texture = use_texture;
        cmd.pso_key = pso_key;
        cmd.color = color;
        cmd.heightmap_texture = heightmap_texture;
        cmd.use_heightmap_displacement = use_heightmap_displacement && heightmap_texture != INVALID_TEXTURE;
        cmd.heightmap_height_scale = heightmap_height_scale;
        cmd.heightmap_height_offset = heightmap_height_offset;
        cmd.heightmap_texel_size = heightmap_texel_size;
        cmd.start_vertex = 0;
        cmd.vertex_count = 0;  // 0 means draw entire mesh
        m_commands.push_back(cmd);
    }

    // Record a range draw command (for multi-material meshes)
    void recordDrawRange(IGPUMesh* gpu_mesh, const glm::mat4& model_matrix,
                         TextureHandle texture, bool use_texture,
                         const PSOKey& pso_key,
                         size_t start_vertex, size_t vertex_count,
                         const glm::vec3& color = glm::vec3(1.0f),
                         float alpha_cutoff = 0.0f,
                         TextureHandle heightmap_texture = INVALID_TEXTURE,
                         bool use_heightmap_displacement = false,
                         float heightmap_height_scale = 1.0f,
                         float heightmap_height_offset = 0.0f,
                         glm::vec2 heightmap_texel_size = glm::vec2(0.0f))
    {
        DrawCommand cmd;
        cmd.gpu_mesh = gpu_mesh;
        cmd.model_matrix = model_matrix;
        cmd.normal_matrix = computeNormalMatrix(model_matrix);
        cmd.texture = texture;
        cmd.use_texture = use_texture;
        cmd.pso_key = pso_key;
        cmd.color = color;
        cmd.alpha_cutoff = alpha_cutoff;
        cmd.heightmap_texture = heightmap_texture;
        cmd.use_heightmap_displacement = use_heightmap_displacement && heightmap_texture != INVALID_TEXTURE;
        cmd.heightmap_height_scale = heightmap_height_scale;
        cmd.heightmap_height_offset = heightmap_height_offset;
        cmd.heightmap_texel_size = heightmap_texel_size;
        cmd.start_vertex = start_vertex;
        cmd.vertex_count = vertex_count;
        m_commands.push_back(cmd);
    }

    // Record a range draw command with PBR material properties
    void recordDrawRangePBR(IGPUMesh* gpu_mesh, const glm::mat4& model_matrix,
                            TextureHandle texture, bool use_texture,
                            const PSOKey& pso_key,
                            size_t start_vertex, size_t vertex_count,
                            const glm::vec3& color, float alpha_cutoff,
                            float metallic, float roughness,
                            const glm::vec3& emissive = glm::vec3(0.0f),
                            TextureHandle heightmap_texture = INVALID_TEXTURE,
                            bool use_heightmap_displacement = false,
                            float heightmap_height_scale = 1.0f,
                            float heightmap_height_offset = 0.0f,
                            glm::vec2 heightmap_texel_size = glm::vec2(0.0f),
                            uint32_t material_flags = 0)
    {
        DrawCommand cmd;
        cmd.gpu_mesh = gpu_mesh;
        cmd.model_matrix = model_matrix;
        cmd.normal_matrix = computeNormalMatrix(model_matrix);
        cmd.texture = texture;
        cmd.use_texture = use_texture;
        cmd.pso_key = pso_key;
        cmd.color = color;
        cmd.alpha_cutoff = alpha_cutoff;
        cmd.heightmap_texture = heightmap_texture;
        cmd.use_heightmap_displacement = use_heightmap_displacement && heightmap_texture != INVALID_TEXTURE;
        cmd.heightmap_height_scale = heightmap_height_scale;
        cmd.heightmap_height_offset = heightmap_height_offset;
        cmd.heightmap_texel_size = heightmap_texel_size;
        cmd.metallic = metallic;
        cmd.roughness = roughness;
        cmd.emissive = emissive;
        cmd.material_flags = material_flags;
        cmd.start_vertex = start_vertex;
        cmd.vertex_count = vertex_count;
        m_commands.push_back(cmd);
    }

    // Sort commands to minimize state changes (by PSO key, then texture).
    // Only use for opaque draws -- transparent draws must maintain back-to-front order.
    void sort()
    {
        auto vecLess = [](const glm::vec3& a, const glm::vec3& b)
        {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        };

        std::sort(m_commands.begin(), m_commands.end(),
            [&](const DrawCommand& a, const DrawCommand& b)
            {
                if (a.pso_key != b.pso_key) return a.pso_key < b.pso_key;
                if (a.use_texture != b.use_texture) return a.use_texture < b.use_texture;
                if (a.texture != b.texture) return a.texture < b.texture;
                if (a.gpu_mesh != b.gpu_mesh) return std::less<IGPUMesh*>{}(a.gpu_mesh, b.gpu_mesh);
                if (a.start_vertex != b.start_vertex) return a.start_vertex < b.start_vertex;
                if (a.vertex_count != b.vertex_count) return a.vertex_count < b.vertex_count;
                if (a.alpha_cutoff != b.alpha_cutoff) return a.alpha_cutoff < b.alpha_cutoff;
                if (a.use_heightmap_displacement != b.use_heightmap_displacement)
                    return a.use_heightmap_displacement < b.use_heightmap_displacement;
                if (a.heightmap_texture != b.heightmap_texture) return a.heightmap_texture < b.heightmap_texture;
                if (a.heightmap_height_scale != b.heightmap_height_scale) return a.heightmap_height_scale < b.heightmap_height_scale;
                if (a.heightmap_height_offset != b.heightmap_height_offset) return a.heightmap_height_offset < b.heightmap_height_offset;
                if (a.heightmap_texel_size.x != b.heightmap_texel_size.x) return a.heightmap_texel_size.x < b.heightmap_texel_size.x;
                if (a.heightmap_texel_size.y != b.heightmap_texel_size.y) return a.heightmap_texel_size.y < b.heightmap_texel_size.y;
                if (a.metallic != b.metallic) return a.metallic < b.metallic;
                if (a.roughness != b.roughness) return a.roughness < b.roughness;
                if (a.material_flags != b.material_flags) return a.material_flags < b.material_flags;
                if (vecLess(a.color, b.color)) return true;
                if (vecLess(b.color, a.color)) return false;
                return vecLess(a.emissive, b.emissive);
            });
    }

    // Append all commands from another buffer (for merging parallel results)
    void append(const RenderCommandBuffer& other)
    {
        m_commands.insert(m_commands.end(),
                          other.m_commands.begin(), other.m_commands.end());
    }

    // Move-append for efficiency
    void append(RenderCommandBuffer&& other)
    {
        if (m_commands.empty())
        {
            m_commands = std::move(other.m_commands);
        }
        else
        {
            m_commands.insert(m_commands.end(),
                              std::make_move_iterator(other.m_commands.begin()),
                              std::make_move_iterator(other.m_commands.end()));
            other.m_commands.clear();
        }
    }

    void clear() { m_commands.clear(); }
    size_t size() const { return m_commands.size(); }
    bool empty() const { return m_commands.empty(); }

    const DrawCommand& operator[](size_t i) const { return m_commands[i]; }
    const std::vector<DrawCommand>& commands() const { return m_commands; }

    // Iterator support for range-based for loops
    auto begin() const { return m_commands.begin(); }
    auto end() const { return m_commands.end(); }

private:
    static glm::mat4 computeNormalMatrix(const glm::mat4& model_matrix)
    {
        glm::mat3 normalMat3 = glm::mat3(model_matrix);
        const float det = glm::determinant(normalMat3);
        if (std::abs(det) > 1e-6f)
            return glm::mat4(glm::transpose(glm::inverse(normalMat3)));
        return glm::mat4(1.0f);
    }

    std::vector<DrawCommand> m_commands;
};
