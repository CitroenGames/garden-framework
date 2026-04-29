#pragma once

#include "LODGenerator.hpp"
#include "Components/mesh.hpp"
#include "Utils/Vertex.hpp"
#include <cstddef>
#include <vector>

namespace Assets {

struct MeshChunkConfig {
    bool enabled = true;
    size_t target_triangles = 8192;
    size_t max_chunks = 3072; // 0 means unlimited
};

struct ChunkedTriangleMesh {
    std::vector<vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MaterialRange> material_ranges;
};

class MeshChunker {
public:
    static ChunkedTriangleMesh buildChunkedIndexedMesh(
        const vertex* vertices,
        size_t vertex_count,
        const std::vector<MaterialRange>& source_ranges,
        const MeshChunkConfig& config);

    static LODMeshData chunkLODMesh(
        const LODMeshData& lod,
        const MeshChunkConfig& config,
        const std::vector<bool>* split_submesh = nullptr);
};

} // namespace Assets
