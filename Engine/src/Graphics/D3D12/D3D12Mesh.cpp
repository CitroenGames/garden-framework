#include "D3D12Mesh.hpp"
#include "Utils/Vertex.hpp"
#include <cstring>

void D3D12Mesh::setD3D12Handles(ID3D12Device* dev, ID3D12CommandQueue* queue,
                                ID3D12CommandAllocator* cmdAlloc, ID3D12GraphicsCommandList* cmdList,
                                ID3D12Fence* fence, HANDLE fenceEvent, UINT64* fenceVal)
{
    device = dev;
    commandQueue = queue;
    uploadCmdAllocator = cmdAlloc;
    uploadCmdList = cmdList;
    uploadFence = fence;
    uploadFenceEvent = fenceEvent;
    uploadFenceValue = fenceVal;
}

ComPtr<ID3D12Resource> D3D12Mesh::uploadToDefaultHeap(const void* data, size_t dataSize)
{
    if (!device || !commandQueue || !data || dataSize == 0) return nullptr;

    // Create default heap resource
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = dataSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(resource.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    // Map and copy
    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, data, dataSize);
    uploadBuffer->Unmap(0, nullptr);

    // Use shared upload infrastructure if available, otherwise fall back to per-mesh
    if (uploadCmdAllocator && uploadCmdList && uploadFence && uploadFenceEvent && uploadFenceValue)
    {
        uploadCmdAllocator->Reset();
        uploadCmdList->Reset(uploadCmdAllocator, nullptr);

        uploadCmdList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, dataSize);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        uploadCmdList->ResourceBarrier(1, &barrier);

        uploadCmdList->Close();
        ID3D12CommandList* lists[] = { uploadCmdList };
        commandQueue->ExecuteCommandLists(1, lists);

        (*uploadFenceValue)++;
        commandQueue->Signal(uploadFence, *uploadFenceValue);
        if (uploadFence->GetCompletedValue() < *uploadFenceValue)
        {
            uploadFence->SetEventOnCompletion(*uploadFenceValue, uploadFenceEvent);
            WaitForSingleObject(uploadFenceEvent, INFINITE);
        }
    }
    else
    {
        // Fallback: create temporary upload infrastructure (legacy path)
        ComPtr<ID3D12CommandAllocator> cmdAlloc;
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        ComPtr<ID3D12Fence> fence;

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmdAlloc.GetAddressOf()));
        if (FAILED(hr)) return nullptr;

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(cmdList.GetAddressOf()));
        if (FAILED(hr)) return nullptr;

        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(hr)) return nullptr;

        cmdList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, dataSize);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);

        commandQueue->Signal(fence.Get(), 1);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence->GetCompletedValue() < 1)
        {
            fence->SetEventOnCompletion(1, event);
            WaitForSingleObject(event, INFINITE);
        }
        CloseHandle(event);
    }

    return resource;
}

void D3D12Mesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!device || !vertices || count == 0) return;

    vertexBuffer.Reset();
    indexBuffer.Reset();
    indexed_ = false;
    index_count_ = 0;

    size_t dataSize = sizeof(vertex) * count;
    vertexBuffer = uploadToDefaultHeap(vertices, dataSize);
    if (!vertexBuffer) return;

    vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vbView.SizeInBytes = static_cast<UINT>(dataSize);
    vbView.StrideInBytes = sizeof(vertex);

    vertex_count = count;
    uploaded = true;
}

void D3D12Mesh::uploadIndexedMeshData(const vertex* vertices, size_t vert_count,
                                       const uint32_t* indices, size_t idx_count)
{
    if (!device || !vertices || vert_count == 0 || !indices || idx_count == 0) return;

    vertexBuffer.Reset();
    indexBuffer.Reset();

    size_t vbSize = sizeof(vertex) * vert_count;
    vertexBuffer = uploadToDefaultHeap(vertices, vbSize);
    if (!vertexBuffer) return;

    vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vbView.SizeInBytes = static_cast<UINT>(vbSize);
    vbView.StrideInBytes = sizeof(vertex);

    size_t ibSize = sizeof(uint32_t) * idx_count;
    indexBuffer = uploadToDefaultHeap(indices, ibSize);
    if (!indexBuffer)
    {
        vertexBuffer.Reset();
        return;
    }

    ibView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
    ibView.SizeInBytes = static_cast<UINT>(ibSize);
    ibView.Format = DXGI_FORMAT_R32_UINT;

    vertex_count = vert_count;
    index_count_ = idx_count;
    indexed_ = true;
    uploaded = true;
}

void D3D12Mesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!device || !vertexBuffer || !vertices || count == 0) return;

    // For simplicity, re-upload the entire buffer
    // A more optimal approach would use a staging buffer and CopyBufferRegion
    size_t dataSize = sizeof(vertex) * count;
    vertexBuffer = uploadToDefaultHeap(vertices, dataSize);
    if (vertexBuffer)
    {
        vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vbView.SizeInBytes = static_cast<UINT>(dataSize);
    }
}
