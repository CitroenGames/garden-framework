#pragma once

#include "Graphics/IGPUMesh.hpp"

class VulkanMesh : public IGPUMesh
{
private:
    size_t vertex_count;
    bool uploaded;

public:
    VulkanMesh();
    ~VulkanMesh() override;

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }
};
