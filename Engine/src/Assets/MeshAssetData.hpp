#pragma once

#include "Utils/Vertex.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Graphics/IGPUMesh.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include <atomic>

namespace Assets {

struct MeshAssetData {
    std::vector<vertex> vertices;

    std::vector<uint32_t> indices;
    bool use_indices = false;

    struct SubMesh {
        size_t start_vertex = 0;
        size_t vertex_count = 0;
        int material_index = -1;
        std::string material_name;

        SubMesh() = default;
        SubMesh(size_t start, size_t count, int mat_idx = -1, const std::string& name = "")
            : start_vertex(start), vertex_count(count), material_index(mat_idx), material_name(name) {}
    };
    std::vector<SubMesh> submeshes;

    glm::vec3 aabb_min{0.0f};
    glm::vec3 aabb_max{0.0f};

    // Thread safety protocol:
    // 1. Main thread sets gpu_mesh pointer
    // 2. Main thread calls uploaded.store(true, release)
    // 3. Worker threads check uploaded.load(acquire) before reading gpu_mesh
    IGPUMesh* gpu_mesh = nullptr;
    std::atomic<bool> uploaded{false};

    std::string source_path;

    // LOD support
    struct MeshLODLevel {
        std::vector<vertex> vertices;
        std::vector<uint32_t> indices;
        IGPUMesh* gpu_mesh = nullptr;
        std::atomic<bool> uploaded{false};
        float screen_threshold = 0.0f;

        MeshLODLevel() = default;
        ~MeshLODLevel() { if (gpu_mesh) { delete gpu_mesh; gpu_mesh = nullptr; } }
        MeshLODLevel(MeshLODLevel&& o) noexcept
            : vertices(std::move(o.vertices)), indices(std::move(o.indices))
            , gpu_mesh(o.gpu_mesh), screen_threshold(o.screen_threshold)
        {
            uploaded.store(o.uploaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
            o.gpu_mesh = nullptr;
            o.uploaded.store(false, std::memory_order_relaxed);
        }
        MeshLODLevel& operator=(MeshLODLevel&& o) noexcept {
            if (this != &o) {
                if (gpu_mesh) delete gpu_mesh;
                vertices = std::move(o.vertices); indices = std::move(o.indices);
                gpu_mesh = o.gpu_mesh; screen_threshold = o.screen_threshold;
                uploaded.store(o.uploaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
                o.gpu_mesh = nullptr;
                o.uploaded.store(false, std::memory_order_relaxed);
            }
            return *this;
        }
        MeshLODLevel(const MeshLODLevel&) = delete;
        MeshLODLevel& operator=(const MeshLODLevel&) = delete;
    };
    std::vector<MeshLODLevel> lod_levels; // LOD1+, LOD0 is the main mesh data
    bool has_lods = false;

    MeshAssetData() = default;

    ~MeshAssetData() {
        if (gpu_mesh) {
            delete gpu_mesh;
            gpu_mesh = nullptr;
        }
    }

    MeshAssetData(MeshAssetData&& other) noexcept
        : vertices(std::move(other.vertices))
        , indices(std::move(other.indices))
        , use_indices(other.use_indices)
        , submeshes(std::move(other.submeshes))
        , aabb_min(other.aabb_min)
        , aabb_max(other.aabb_max)
        , gpu_mesh(other.gpu_mesh)
        , source_path(std::move(other.source_path))
        , lod_levels(std::move(other.lod_levels))
        , has_lods(other.has_lods)
    {
        uploaded.store(other.uploaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.gpu_mesh = nullptr;
        other.uploaded.store(false, std::memory_order_relaxed);
        other.has_lods = false;
    }

    MeshAssetData& operator=(MeshAssetData&& other) noexcept {
        if (this != &other) {
            if (gpu_mesh) delete gpu_mesh;

            vertices = std::move(other.vertices);
            indices = std::move(other.indices);
            use_indices = other.use_indices;
            submeshes = std::move(other.submeshes);
            aabb_min = other.aabb_min;
            aabb_max = other.aabb_max;
            gpu_mesh = other.gpu_mesh;
            uploaded.store(other.uploaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
            source_path = std::move(other.source_path);
            lod_levels = std::move(other.lod_levels);
            has_lods = other.has_lods;

            other.gpu_mesh = nullptr;
            other.uploaded.store(false, std::memory_order_relaxed);
            other.has_lods = false;
        }
        return *this;
    }

    MeshAssetData(const MeshAssetData&) = delete;
    MeshAssetData& operator=(const MeshAssetData&) = delete;

    void freeVertices() {
        std::vector<vertex>().swap(vertices);
        std::vector<uint32_t>().swap(indices);
    }

    void computeBounds() {
        if (vertices.empty()) return;

        aabb_min = glm::vec3(vertices[0].vx, vertices[0].vy, vertices[0].vz);
        aabb_max = aabb_min;

        for (const auto& v : vertices) {
            aabb_min.x = std::min(aabb_min.x, v.vx);
            aabb_min.y = std::min(aabb_min.y, v.vy);
            aabb_min.z = std::min(aabb_min.z, v.vz);
            aabb_max.x = std::max(aabb_max.x, v.vx);
            aabb_max.y = std::max(aabb_max.y, v.vy);
            aabb_max.z = std::max(aabb_max.z, v.vz);
        }
    }

    size_t getVertexCount() const { return vertices.size(); }
    size_t getIndexCount() const { return indices.size(); }
    size_t getTriangleCount() const {
        return use_indices ? indices.size() / 3 : vertices.size() / 3;
    }
};

} // namespace Assets
