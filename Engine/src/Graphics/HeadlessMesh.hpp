#pragma once

#include "Graphics/IGPUMesh.hpp"

class HeadlessMesh : public IGPUMesh
{
private:
    uint64_t upload_calls = 0;
    uint64_t upload_bytes = 0;
    size_t vertex_count;
    bool uploaded;

public:
    HeadlessMesh();
    ~HeadlessMesh() override;

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    uint64_t getUploadCalls() const { return upload_calls; }
    uint64_t getUploadBytes() const { return upload_bytes; }
    size_t getVertexCount() const override { return vertex_count; }
};
