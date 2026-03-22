#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <cstddef>

// Forward declaration - Metal types hidden in .mm
struct MetalMeshImpl;

class MetalMesh : public IGPUMesh
{
public:
    MetalMesh();
    ~MetalMesh() override;

    // Set Metal handles (called by MetalRenderAPI::createMesh)
    void setDevice(void* device); // id<MTLDevice> as void*
    void setCommandQueue(void* queue); // id<MTLCommandQueue> as void*

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override;
    size_t getVertexCount() const override;

    // Metal-specific
    void* getVertexBuffer() const; // Returns id<MTLBuffer> as void*
    void cleanup();

private:
    struct Impl;
    Impl* pImpl;
};
