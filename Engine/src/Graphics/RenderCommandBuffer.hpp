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
    // Packed pipeline variant bits (top 16 of the 64-bit sort key).
    // Bit15 shadow, bit14 depth_only, bit13 lighting, bit12 alpha_test,
    // bits11-10 blend, bits9-8 cull. Numeric order matches operator< priority.
    uint16_t pipelineBits() const
    {
        uint16_t b = 0;
        if (shadow) b |= (1u << 15);
        if (depth_only) b |= (1u << 14);
        if (lighting) b |= (1u << 13);
        if (alpha_test) b |= (1u << 12);
        b |= (uint16_t)((uint16_t)blend & 0x3u) << 10;
        b |= (uint16_t)((uint16_t)cull & 0x3u) << 8;
        return b;
    }

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

    // Packed 64-bit sort key: pipeline(16) | material(16) | lighting(12) |
    // geometry(8) | depth(12). Numeric order matches field priority, so a
    // plain integer sort groups draws to minimize state changes; the depth
    // bucket (front-to-back, near = 0) only orders within the same group.
    uint64_t packedSortKey(uint16_t depthBucket = 0) const
    {
        uint64_t pipeline = (uint64_t)pso_key.pipelineBits();
        uint64_t material = (uint64_t)(material_flags & 0xFFFFu);
        uint64_t lighting = (uint64_t)((pso_key.lighting ? 0x800u : 0u) |
            (((uint64_t)pso_key.blend & 0x3u) << 8) |
            ((use_texture ? 1u : 0u) << 7) |
            ((pso_key.shadow ? 1u : 0u) << 6));
        uint64_t geometry = (uint64_t)((((uint64_t)pso_key.cull & 0x3u) << 6) |
            ((pso_key.alpha_test ? 1u : 0u) << 5) |
            ((use_heightmap_displacement ? 1u : 0u) << 4) |
            ((vertex_count > 0 ? 1u : 0u) << 3));
        return (pipeline << 48) | (material << 32) | (lighting << 20) |
            (geometry << 12) | (uint64_t)(depthBucket & 0xFFFu);
    }

    // For range draws (renderMeshRange). If vertex_count == 0, draw entire mesh.
    size_t start_vertex = 0;
    size_t vertex_count = 0;
};

// A draw may join a merged instanced draw only when every per-draw value
// the shader reads from its per-object data is shared by the whole run, so
// the sole remaining difference between members is their transform. Shadow,
// depth-only and blended draws keep individual draws, matching the order
// guarantees those draws rely on.
inline bool drawSupportsInstancing(const DrawCommand& cmd)
{
    return !cmd.pso_key.shadow
        && !cmd.pso_key.depth_only
        && cmd.pso_key.blend == BlendMode::None;
}

inline bool drawInstanceCompatible(const DrawCommand& a, const DrawCommand& b)
{
    auto vec3Equal = [](const glm::vec3& x, const glm::vec3& y)
    {
        return x.x == y.x && x.y == y.y && x.z == y.z;
    };
    return drawSupportsInstancing(a)
        && drawSupportsInstancing(b)
        && a.gpu_mesh == b.gpu_mesh
        && a.pso_key == b.pso_key
        && a.use_texture == b.use_texture
        && a.texture == b.texture
        && a.use_heightmap_displacement == b.use_heightmap_displacement
        && a.heightmap_texture == b.heightmap_texture
        && a.heightmap_height_scale == b.heightmap_height_scale
        && a.heightmap_height_offset == b.heightmap_height_offset
        && a.heightmap_texel_size.x == b.heightmap_texel_size.x
        && a.heightmap_texel_size.y == b.heightmap_texel_size.y
        && a.start_vertex == b.start_vertex
        && a.vertex_count == b.vertex_count
        && a.alpha_cutoff == b.alpha_cutoff
        && a.metallic == b.metallic
        && a.roughness == b.roughness
        && a.material_flags == b.material_flags
        && vec3Equal(a.color, b.color)
        && vec3Equal(a.emissive, b.emissive);
}

// Length of the merged run beginning at first, always at least one.
inline size_t instancedRunLength(const std::vector<DrawCommand>& cmds, size_t first)
{
    size_t length = 1;
    if (first >= cmds.size() || !drawSupportsInstancing(cmds[first]))
        return length;
    while (first + length < cmds.size() &&
           drawInstanceCompatible(cmds[first], cmds[first + length]))
        ++length;
    return length;
}

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

    // Full state ordering shared by both sort orders: packed key first,
    // then every remaining value a backend binds or a merged run compares,
    // so draws that share all state stay adjacent under either order.
    static bool drawStateLess(const DrawCommand& a, const DrawCommand& b)
    {
        auto vecLess = [](const glm::vec3& x, const glm::vec3& y)
        {
            if (x.x != y.x) return x.x < y.x;
            if (x.y != y.y) return x.y < y.y;
            return x.z < y.z;
        };

        uint64_t ka = a.packedSortKey();
        uint64_t kb = b.packedSortKey();
        if (ka != kb) return ka < kb;
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
    }

    // Sort commands to minimize state changes (by PSO key, then texture).
    // Only use for opaque draws -- transparent draws must maintain back-to-front order.
    void sort()
    {
        std::sort(m_commands.begin(), m_commands.end(),
            [](const DrawCommand& a, const DrawCommand& b) { return drawStateLess(a, b); });
    }

    // Depth-bucketed sort for opaque draws: groups by the same full state
    // order as sort(), then orders front-to-back within each state group
    // using a 12-bit view-space depth bucket (near = 0), so state grouping
    // and merged runs survive while nearer draws in a group submit first.
    // Transparent draws must NOT use this -- they require back-to-front
    // submission order.
    void sortWithViewDepth(const glm::mat4& viewMatrix)
    {
        if (m_commands.empty()) return;
        std::vector<float> depths(m_commands.size());
        float minD = 0.0f, maxD = 0.0f;
        for (size_t i = 0; i < m_commands.size(); i++)
        {
            glm::vec3 pos = glm::vec3(m_commands[i].model_matrix[3]);
            float d = -(glm::vec3(viewMatrix * glm::vec4(pos, 1.0f)).z);
            depths[i] = d;
            if (i == 0) { minD = d; maxD = d; }
            else { if (d < minD) minD = d; if (d > maxD) maxD = d; }
        }
        std::vector<uint16_t> buckets(m_commands.size(), 0);
        if (maxD > minD)
        {
            for (size_t i = 0; i < depths.size(); i++)
            {
                float t = (depths[i] - minD) / (maxD - minD);
                buckets[i] = (uint16_t)(t * 4095.0f + 0.5f);
            }
        }
        // Flattened ordering fields in one contiguous array, compared in
        // the same order as drawStateLess with the depth bucket last.
        struct SortEntry
        {
            uint64_t packed = 0;
            uint64_t mesh = 0;
            size_t start_vertex = 0;
            size_t vertex_count = 0;
            TextureHandle texture = INVALID_TEXTURE;
            TextureHandle heightmap_texture = INVALID_TEXTURE;
            uint32_t material_flags = 0;
            float alpha_cutoff = 0.0f;
            float heightmap_height_scale = 0.0f;
            float heightmap_height_offset = 0.0f;
            float heightmap_texel_x = 0.0f;
            float heightmap_texel_y = 0.0f;
            float metallic = 0.0f;
            float roughness = 0.0f;
            glm::vec3 color = glm::vec3(0.0f);
            glm::vec3 emissive = glm::vec3(0.0f);
            uint16_t bucket = 0;
            uint32_t index = 0;
            bool use_texture = false;
            bool use_heightmap = false;
        };
        std::vector<SortEntry> entries(m_commands.size());
        for (size_t i = 0; i < m_commands.size(); i++)
        {
            const DrawCommand& c = m_commands[i];
            SortEntry& e = entries[i];
            e.packed = c.packedSortKey();
            e.mesh = (uint64_t)(uintptr_t)c.gpu_mesh;
            e.start_vertex = c.start_vertex;
            e.vertex_count = c.vertex_count;
            e.texture = c.texture;
            e.heightmap_texture = c.heightmap_texture;
            e.material_flags = c.material_flags;
            e.alpha_cutoff = c.alpha_cutoff;
            e.heightmap_height_scale = c.heightmap_height_scale;
            e.heightmap_height_offset = c.heightmap_height_offset;
            e.heightmap_texel_x = c.heightmap_texel_size.x;
            e.heightmap_texel_y = c.heightmap_texel_size.y;
            e.metallic = c.metallic;
            e.roughness = c.roughness;
            e.color = c.color;
            e.emissive = c.emissive;
            e.bucket = buckets[i];
            e.index = (uint32_t)i;
            e.use_texture = c.use_texture;
            e.use_heightmap = c.use_heightmap_displacement;
        }
        std::sort(entries.begin(), entries.end(),
            [](const SortEntry& a, const SortEntry& b)
            {
                if (a.packed != b.packed) return a.packed < b.packed;
                if (a.use_texture != b.use_texture) return a.use_texture < b.use_texture;
                if (a.texture != b.texture) return a.texture < b.texture;
                if (a.mesh != b.mesh) return a.mesh < b.mesh;
                if (a.start_vertex != b.start_vertex) return a.start_vertex < b.start_vertex;
                if (a.vertex_count != b.vertex_count) return a.vertex_count < b.vertex_count;
                if (a.alpha_cutoff != b.alpha_cutoff) return a.alpha_cutoff < b.alpha_cutoff;
                if (a.use_heightmap != b.use_heightmap) return a.use_heightmap < b.use_heightmap;
                if (a.heightmap_texture != b.heightmap_texture) return a.heightmap_texture < b.heightmap_texture;
                if (a.heightmap_height_scale != b.heightmap_height_scale) return a.heightmap_height_scale < b.heightmap_height_scale;
                if (a.heightmap_height_offset != b.heightmap_height_offset) return a.heightmap_height_offset < b.heightmap_height_offset;
                if (a.heightmap_texel_x != b.heightmap_texel_x) return a.heightmap_texel_x < b.heightmap_texel_x;
                if (a.heightmap_texel_y != b.heightmap_texel_y) return a.heightmap_texel_y < b.heightmap_texel_y;
                if (a.metallic != b.metallic) return a.metallic < b.metallic;
                if (a.roughness != b.roughness) return a.roughness < b.roughness;
                if (a.material_flags != b.material_flags) return a.material_flags < b.material_flags;
                if (a.color.x != b.color.x) return a.color.x < b.color.x;
                if (a.color.y != b.color.y) return a.color.y < b.color.y;
                if (a.color.z != b.color.z) return a.color.z < b.color.z;
                if (a.emissive.x != b.emissive.x) return a.emissive.x < b.emissive.x;
                if (a.emissive.y != b.emissive.y) return a.emissive.y < b.emissive.y;
                if (a.emissive.z != b.emissive.z) return a.emissive.z < b.emissive.z;
                return a.bucket < b.bucket;
            });
        std::vector<DrawCommand> sorted;
        sorted.reserve(m_commands.size());
        for (const SortEntry& e : entries) sorted.push_back(m_commands[e.index]);
        m_commands = std::move(sorted);
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
