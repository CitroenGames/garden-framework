#include "D3D11Mesh.hpp"
#include "Utils/Vertex.hpp"
#include <cstring>

D3D11Mesh::D3D11Mesh()
    : vertex_count(0)
    , index_count_(0)
    , uploaded(false)
    , indexed_(false)
    , device(nullptr)
    , context(nullptr)
{
}

D3D11Mesh::~D3D11Mesh()
{
    // ComPtr handles release automatically
}

void D3D11Mesh::setD3D11Handles(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    device = dev;
    context = ctx;
}

void D3D11Mesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!device || !vertices || count == 0)
        return;

    // Release existing buffers
    vertexBuffer.Reset();
    indexBuffer.Reset();
    indexed_ = false;
    index_count_ = 0;

    // Create vertex buffer
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = static_cast<UINT>(sizeof(vertex) * count);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    initData.SysMemPitch = 0;
    initData.SysMemSlicePitch = 0;

    HRESULT hr = device->CreateBuffer(&bd, &initData, vertexBuffer.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        vertex_count = count;
        uploaded = true;
    }
}

void D3D11Mesh::uploadIndexedMeshData(const vertex* vertices, size_t vert_count,
                                       const uint32_t* indices, size_t idx_count)
{
    if (!device || !vertices || vert_count == 0 || !indices || idx_count == 0)
        return;

    // Release existing buffers
    vertexBuffer.Reset();
    indexBuffer.Reset();

    // Create vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = static_cast<UINT>(sizeof(vertex) * vert_count);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;

    HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vertexBuffer.GetAddressOf());
    if (FAILED(hr)) return;

    // Create index buffer
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * idx_count);
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices;

    hr = device->CreateBuffer(&ibDesc, &ibData, indexBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        vertexBuffer.Reset();
        return;
    }

    vertex_count = vert_count;
    index_count_ = idx_count;
    indexed_ = true;
    uploaded = true;
}

void D3D11Mesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!context || !vertexBuffer || !vertices || count == 0)
        return;

    // For partial updates, we need to use UpdateSubresource or Map/Unmap
    // UpdateSubresource is simpler for non-dynamic buffers
    D3D11_BOX box = {};
    box.left = static_cast<UINT>(offset * sizeof(vertex));
    box.right = static_cast<UINT>((offset + count) * sizeof(vertex));
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;

    context->UpdateSubresource(vertexBuffer.Get(), 0, &box, vertices, 0, 0);
}

void D3D11Mesh::bind() const
{
    if (!context || !vertexBuffer)
        return;

    UINT stride = sizeof(vertex);
    UINT offset = 0;
    ID3D11Buffer* buffers[] = { vertexBuffer.Get() };
    context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);

    if (indexed_ && indexBuffer)
    {
        context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
}
