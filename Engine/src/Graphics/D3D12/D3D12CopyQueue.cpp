#include "D3D12CopyQueue.hpp"
#include "D3D12BarrierBatch.hpp"
#include "Utils/Log.hpp"

bool D3D12CopyQueue::init(ID3D12Device* device)
{
    m_device = device;

    // Create a dedicated copy command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_copyQueue.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12 CopyQueue] Failed to create copy command queue (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                         IID_PPV_ARGS(m_allocator.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12 CopyQueue] Failed to create command allocator (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                    m_allocator.Get(), nullptr,
                                    IID_PPV_ARGS(m_cmdList.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12 CopyQueue] Failed to create command list (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    m_cmdList->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12 CopyQueue] Failed to create fence (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) return false;

    m_fenceValue = 0;
    m_lastSubmittedFence = 0;

    LOG_ENGINE_TRACE("[D3D12 CopyQueue] Initialized");
    return true;
}

void D3D12CopyQueue::shutdown()
{
    flushSync();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_stagingBuffers.clear();
    m_pendingTransitions.clear();
    m_cmdList.Reset();
    m_allocator.Reset();
    m_fence.Reset();
    m_copyQueue.Reset();
    m_device = nullptr;
}

void D3D12CopyQueue::ensureOpen()
{
    if (m_isOpen) return;

    // Wait for previous submission to complete before resetting the allocator
    if (m_lastSubmittedFence > 0 && m_fence->GetCompletedValue() < m_lastSubmittedFence)
    {
        m_fence->SetEventOnCompletion(m_lastSubmittedFence, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_allocator->Reset();
    m_cmdList->Reset(m_allocator.Get(), nullptr);
    m_isOpen = true;
}

ID3D12GraphicsCommandList* D3D12CopyQueue::getCommandList()
{
    ensureOpen();
    return m_cmdList.Get();
}

void D3D12CopyQueue::retainStagingBuffer(ComPtr<ID3D12Resource> buffer)
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    m_stagingBuffers.push_back(std::move(buffer));
}

void D3D12CopyQueue::addPendingTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES targetState)
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    m_pendingTransitions.push_back({ resource, targetState });
}

void D3D12CopyQueue::submit()
{
    if (!m_isOpen) return;

    m_cmdList->Close();
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_copyQueue->ExecuteCommandLists(1, lists);

    m_fenceValue++;
    m_lastSubmittedFence = m_fenceValue;
    m_copyQueue->Signal(m_fence.Get(), m_fenceValue);

    m_isOpen = false;
}

void D3D12CopyQueue::waitOnGraphicsQueue(ID3D12CommandQueue* graphicsQueue)
{
    if (m_lastSubmittedFence == 0) return;
    // GPU-side wait: the graphics queue pauses until the copy queue fence is reached.
    // The CPU does NOT block here.
    graphicsQueue->Wait(m_fence.Get(), m_lastSubmittedFence);
}

void D3D12CopyQueue::applyPendingTransitions(ID3D12GraphicsCommandList* graphicsCmdList)
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    if (m_pendingTransitions.empty()) return;

    BarrierBatch batch;
    for (auto& pt : m_pendingTransitions)
    {
        batch.add(pt.resource, D3D12_RESOURCE_STATE_COMMON, pt.targetState);
        // Flush in batches of 16 if we have many transitions
        if (batch.count() >= BarrierBatch::MAX_BARRIERS)
            batch.flush(graphicsCmdList);
    }
    batch.flush(graphicsCmdList);
    m_pendingTransitions.clear();
}

void D3D12CopyQueue::flushSync()
{
    if (m_isOpen)
        submit();

    if (m_lastSubmittedFence > 0 && m_fence && m_fence->GetCompletedValue() < m_lastSubmittedFence)
    {
        m_fence->SetEventOnCompletion(m_lastSubmittedFence, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    {
        std::lock_guard<std::mutex> lock(m_stagingMutex);
        m_stagingBuffers.clear();
        m_pendingTransitions.clear();
    }
}

void D3D12CopyQueue::releaseStagingBuffers()
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    // Only release if the GPU is done with them
    if (m_lastSubmittedFence > 0 && m_fence && m_fence->GetCompletedValue() >= m_lastSubmittedFence)
    {
        m_stagingBuffers.clear();
    }
}
