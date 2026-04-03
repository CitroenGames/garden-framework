#include "NavMeshGenerator.hpp"
#include "Components/Components.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <cmath>
#include <chrono>

namespace Navigation
{

// ─── Geometry extraction ─────────────────────────────────────────────────────

std::vector<NavMeshGenerator::RawTriangle>
NavMeshGenerator::extractWorldTriangles(entt::registry& registry)
{
    std::vector<RawTriangle> out;

    // Prefer ColliderComponent mesh, fall back to MeshComponent
    auto view = registry.view<TransformComponent>();

    for (auto entity : view)
    {
        const mesh* m = nullptr;

        if (registry.all_of<ColliderComponent>(entity))
        {
            auto& col = registry.get<ColliderComponent>(entity);
            m = col.get_mesh();
        }

        if (!m && registry.all_of<MeshComponent>(entity))
        {
            auto& mc = registry.get<MeshComponent>(entity);
            if (mc.m_mesh && mc.m_mesh->is_valid)
                m = mc.m_mesh.get();
        }

        if (!m || !m->is_valid || !m->vertices || m->vertices_len < 3)
            continue;

        // Skip dynamic entities (those with rigidbodies and gravity)
        if (registry.all_of<RigidBodyComponent>(entity))
        {
            auto& rb = registry.get<RigidBodyComponent>(entity);
            if (rb.apply_gravity)
                continue;
        }

        glm::mat4 transform = registry.get<TransformComponent>(entity).getTransformMatrix();
        glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(transform)));

        for (size_t i = 0; i + 2 < m->vertices_len; i += 3)
        {
            RawTriangle tri;
            for (int j = 0; j < 3; j++)
            {
                const vertex& vtx = m->vertices[i + j];
                glm::vec4 world_pos = transform * glm::vec4(vtx.vx, vtx.vy, vtx.vz, 1.0f);
                tri.v[j] = glm::vec3(world_pos);
            }

            glm::vec3 edge1 = tri.v[1] - tri.v[0];
            glm::vec3 edge2 = tri.v[2] - tri.v[0];
            glm::vec3 cross = glm::cross(edge1, edge2);
            float len = glm::length(cross);

            // Skip degenerate triangles
            if (len < 1e-6f)
                continue;

            tri.normal = cross / len;
            out.push_back(tri);
        }
    }

    return out;
}

// ─── Slope filtering ─────────────────────────────────────────────────────────

std::vector<NavMeshGenerator::RawTriangle>
NavMeshGenerator::filterBySlope(const std::vector<RawTriangle>& tris, float max_slope_degrees)
{
    std::vector<RawTriangle> out;
    out.reserve(tris.size() / 2);

    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float max_cos = std::cos(glm::radians(max_slope_degrees));

    for (auto& tri : tris)
    {
        float d = glm::dot(tri.normal, up);
        if (d >= max_cos)
            out.push_back(tri);
    }

    return out;
}

// ─── Adjacency building ─────────────────────────────────────────────────────

struct SnappedVertex
{
    int32_t x, y, z;

    bool operator==(const SnappedVertex& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator<(const SnappedVertex& o) const
    {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

struct EdgeKey
{
    SnappedVertex a, b; // a <= b (canonical ordering)

    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash
{
    size_t operator()(const EdgeKey& k) const
    {
        // FNV-1a over the 6 ints
        size_t h = 14695981039346656037ULL;
        auto mix = [&](int32_t v) {
            h ^= static_cast<size_t>(v);
            h *= 1099511628211ULL;
        };
        mix(k.a.x); mix(k.a.y); mix(k.a.z);
        mix(k.b.x); mix(k.b.y); mix(k.b.z);
        return h;
    }
};

struct EdgeEntry
{
    int32_t tri_index;
    int32_t edge_index; // 0, 1, or 2
};

void NavMeshGenerator::buildAdjacency(NavMesh& navmesh, float merge_distance)
{
    float eps = merge_distance;
    if (eps <= 0.0f) eps = 0.001f;

    auto snap = [eps](const glm::vec3& v) -> SnappedVertex {
        return {
            static_cast<int32_t>(std::round(v.x / eps)),
            static_cast<int32_t>(std::round(v.y / eps)),
            static_cast<int32_t>(std::round(v.z / eps))
        };
    };

    auto makeEdgeKey = [](SnappedVertex a, SnappedVertex b) -> EdgeKey {
        if (b < a) { auto t = a; a = b; b = t; }
        return {a, b};
    };

    std::unordered_map<EdgeKey, std::vector<EdgeEntry>, EdgeKeyHash> edge_map;
    edge_map.reserve(navmesh.triangles.size() * 3);

    // Edge indices: edge 0 = v0-v1, edge 1 = v1-v2, edge 2 = v2-v0
    static const int edge_verts[3][2] = {{0, 1}, {1, 2}, {2, 0}};

    for (int32_t ti = 0; ti < static_cast<int32_t>(navmesh.triangles.size()); ti++)
    {
        auto& tri = navmesh.triangles[ti];
        tri.centroid = (tri.vertices[0] + tri.vertices[1] + tri.vertices[2]) / 3.0f;

        for (int ei = 0; ei < 3; ei++)
        {
            SnappedVertex sa = snap(tri.vertices[edge_verts[ei][0]]);
            SnappedVertex sb = snap(tri.vertices[edge_verts[ei][1]]);
            EdgeKey key = makeEdgeKey(sa, sb);
            edge_map[key].push_back({ti, ei});
        }
    }

    // Link adjacent triangles
    for (auto& [key, entries] : edge_map)
    {
        if (entries.size() == 2)
        {
            auto& a = entries[0];
            auto& b = entries[1];
            navmesh.triangles[a.tri_index].neighbors[a.edge_index] = b.tri_index;
            navmesh.triangles[a.tri_index].shared_edge[a.edge_index] = b.edge_index;
            navmesh.triangles[b.tri_index].neighbors[b.edge_index] = a.tri_index;
            navmesh.triangles[b.tri_index].shared_edge[b.edge_index] = a.edge_index;
        }
    }
}

// ─── Main generation ─────────────────────────────────────────────────────────

NavMesh NavMeshGenerator::generate(entt::registry& registry, const NavMeshConfig& config,
                                   GenerationStats* stats)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    NavMesh navmesh;
    navmesh.config = config;

    // Step 1: Extract all world-space triangles
    auto raw = extractWorldTriangles(registry);

    int source_count = static_cast<int>(raw.size());

    // Step 2: Filter by slope
    auto walkable = filterBySlope(raw, config.max_slope_angle);

    // Step 3: Build NavTriangle array
    navmesh.triangles.resize(walkable.size());
    for (size_t i = 0; i < walkable.size(); i++)
    {
        auto& nt = navmesh.triangles[i];
        nt.vertices[0] = walkable[i].v[0];
        nt.vertices[1] = walkable[i].v[1];
        nt.vertices[2] = walkable[i].v[2];
        nt.normal = walkable[i].normal;
    }

    // Step 4: Build adjacency
    buildAdjacency(navmesh, config.merge_distance);

    navmesh.valid = !navmesh.triangles.empty();

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    if (stats)
    {
        stats->source_triangles = source_count;
        stats->walkable_triangles = static_cast<int>(navmesh.triangles.size());
        stats->time_ms = ms;
    }

    return navmesh;
}

} // namespace Navigation
