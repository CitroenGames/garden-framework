#pragma once

#include "Utils/Vertex.hpp"
#include <cstddef>
#include <cstdint>

class IGPUMesh
{
public:
    virtual ~IGPUMesh() = default;

    // Upload mesh data to GPU
    virtual void uploadMeshData(const vertex* vertices, size_t count) = 0;

    // Upload indexed mesh data to GPU
    virtual void uploadIndexedMeshData(const vertex* vertices, size_t vertex_count,
                                       const uint32_t* indices, size_t index_count)
    {
        // Default: fall back to non-indexed upload
        uploadMeshData(vertices, vertex_count);
    }

    // Update mesh data (for dynamic meshes)
    virtual void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) = 0;

    // Check if mesh has been uploaded
    virtual bool isUploaded() const = 0;

    // Get vertex count
    virtual size_t getVertexCount() const = 0;

    // Index buffer support
    virtual bool isIndexed() const { return false; }
    virtual size_t getIndexCount() const { return 0; }
};
