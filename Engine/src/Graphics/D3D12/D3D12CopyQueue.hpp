#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>

using Microsoft::WRL::ComPtr;

// Dedicated copy queue for async texture uploads.
// Copies execute on a separate hardware queue in parallel with rendering.
// Resources are created in COMMON state, implicitly promoted to COPY_DEST on the
// copy queue, and decay back to COMMON after ExecuteCommandLists. The graphics
// queue then transitions them to the target state before use.
class D3D12CopyQueue
{
public:
    bool init(ID3D12Device* device);
    void shutdown();

    // Get the copy command list for recording CopyTextureRegion/CopyBufferRegion.
    // Lazily opens the command list on first call after submit/init.
    ID3D12GraphicsCommandList* getCommandList();

    // Keep a staging buffer alive until the copy completes.
    void retainStagingBuffer(ComPtr<ID3D12Resource> buffer);

    // Register a resource that needs a COMMON -> targetState transition
    // on the graphics queue after the copy completes.
    void addPendingTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES targetState);

    // Submit accumulated copy work to the copy queue (non-blocking).
    void submit();

    // Insert a GPU-side wait on the graphics queue for the last submitted copy batch.
    // The graphics queue pauses until the copy queue finishes — the CPU does NOT block.
    void waitOnGraphicsQueue(ID3D12CommandQueue* graphicsQueue);

    // Apply pending COMMON -> target transitions on the graphics command list.
    // Call after waitOnGraphicsQueue, before using the uploaded resources.
    void applyPendingTransitions(ID3D12GraphicsCommandList* graphicsCmdList);

    // Synchronous CPU wait for all submitted work, then release staging buffers.
    void flushSync();

    // Release staging buffers from completed copies.
    void releaseStagingBuffers();

    bool hasPendingWork() const { return m_isOpen; }
    bool hasSubmittedWork() const { return m_lastSubmittedFence > 0 && !m_pendingTransitions.empty(); }

private:
    struct PendingTransition
    {
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES targetState;
    };

    ID3D12Device* m_device = nullptr;
    ComPtr<ID3D12CommandQueue> m_copyQueue;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    UINT64 m_lastSubmittedFence = 0;
    bool m_isOpen = false;

    std::vector<ComPtr<ID3D12Resource>> m_stagingBuffers;
    std::vector<PendingTransition> m_pendingTransitions;
    std::mutex m_stagingMutex; // Guards m_stagingBuffers and m_pendingTransitions

    void ensureOpen();
};
