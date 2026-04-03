#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class D3D11Mesh : public IGPUMesh
{
private:
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    size_t vertex_count;
    size_t index_count_;
    bool uploaded;
    bool indexed_;

    // Reference to D3D11 device/context (set by D3D11RenderAPI::createMesh)
    ID3D11Device* device;
    ID3D11DeviceContext* context;

public:
    D3D11Mesh();
    ~D3D11Mesh() override;

    void setD3D11Handles(ID3D11Device* dev, ID3D11DeviceContext* ctx);

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void uploadIndexedMeshData(const vertex* vertices, size_t vertex_count,
                               const uint32_t* indices, size_t index_count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }
    bool isIndexed() const override { return indexed_; }
    size_t getIndexCount() const override { return index_count_; }

    // D3D11 specific methods
    void bind() const;
    ID3D11Buffer* getVertexBuffer() const { return vertexBuffer.Get(); }
    ID3D11Buffer* getIndexBuffer() const { return indexBuffer.Get(); }
};
