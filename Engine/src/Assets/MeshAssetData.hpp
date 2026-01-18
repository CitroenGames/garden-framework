#pragma once

#include "Utils/Vertex.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Graphics/IGPUMesh.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

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

    IGPUMesh* gpu_mesh = nullptr;
    bool uploaded = false;

    std::string source_path;

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
        , uploaded(other.uploaded)
        , source_path(std::move(other.source_path))
    {
        other.gpu_mesh = nullptr;
        other.uploaded = false;
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
            uploaded = other.uploaded;
            source_path = std::move(other.source_path);

            other.gpu_mesh = nullptr;
            other.uploaded = false;
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
