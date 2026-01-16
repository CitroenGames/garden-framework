#pragma once

#include "Utils/Vertex.hpp"
#include <cstddef>

class IGPUMesh
{
public:
    virtual ~IGPUMesh() = default;

    // Upload mesh data to GPU
    virtual void uploadMeshData(const vertex* vertices, size_t count) = 0;

    // Update mesh data (for dynamic meshes)
    virtual void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) = 0;

    // Check if mesh has been uploaded
    virtual bool isUploaded() const = 0;

    // Get vertex count
    virtual size_t getVertexCount() const = 0;
};
