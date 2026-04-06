#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class D3D12Mesh : public IGPUMesh
{
private:
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW ibView = {};
    size_t vertex_count = 0;
    size_t index_count_ = 0;
    bool uploaded = false;
    bool indexed_ = false;

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;

    ComPtr<ID3D12Resource> uploadToDefaultHeap(const void* data, size_t dataSize);

public:
    D3D12Mesh() = default;
    ~D3D12Mesh() override = default;

    void setD3D12Handles(ID3D12Device* dev, ID3D12CommandQueue* queue);

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void uploadIndexedMeshData(const vertex* vertices, size_t vertex_count,
                               const uint32_t* indices, size_t index_count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }
    bool isIndexed() const override { return indexed_; }
    size_t getIndexCount() const override { return index_count_; }

    // D3D12 specific
    const D3D12_VERTEX_BUFFER_VIEW& getVertexBufferView() const { return vbView; }
    const D3D12_INDEX_BUFFER_VIEW& getIndexBufferView() const { return ibView; }
};
