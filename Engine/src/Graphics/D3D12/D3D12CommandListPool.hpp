#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <atomic>

using Microsoft::WRL::ComPtr;

// Pool of {ID3D12CommandAllocator, ID3D12GraphicsCommandList} pairs for
// parallel command list recording. Each worker thread gets one entry
// during parallel replay; all command lists are submitted together.
//
// Usage:
//   1. Call resetAll() at frame start (after GPU finishes previous frame)
//   2. Call acquire() per worker thread to get a command list
//   3. Worker records commands into its command list
//   4. Collect all command lists and submit via ExecuteCommandLists()
//
// Each command list must independently set: root signature, descriptor heaps,
// render target, viewport, scissor rect (state does not carry across D3D12
// command lists).
class D3D12CommandListPool
{
public:
    struct Entry
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        std::atomic<bool> in_use{false};

        Entry() = default;
        Entry(Entry&& other) noexcept
            : allocator(std::move(other.allocator))
            , cmdList(std::move(other.cmdList))
        {
            in_use.store(other.in_use.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        Entry& operator=(Entry&& other) noexcept
        {
            allocator = std::move(other.allocator);
            cmdList = std::move(other.cmdList);
            in_use.store(other.in_use.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
    };

    // Initialize the pool with N entries (typically hardware_concurrency).
    bool init(ID3D12Device* device, uint32_t count,
              D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        m_entries.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            HRESULT hr = device->CreateCommandAllocator(
                type, IID_PPV_ARGS(m_entries[i].allocator.GetAddressOf()));
            if (FAILED(hr)) return false;

            hr = device->CreateCommandList(
                0, type, m_entries[i].allocator.Get(), nullptr,
                IID_PPV_ARGS(m_entries[i].cmdList.GetAddressOf()));
            if (FAILED(hr)) return false;

            // Command lists are created in recording state; close them
            m_entries[i].cmdList->Close();
            m_entries[i].in_use.store(false, std::memory_order_relaxed);
        }
        return true;
    }

    // Reset all allocators and mark all entries as available.
    // Call at frame start after the GPU fence signals for this frame.
    void resetAll()
    {
        for (auto& entry : m_entries)
        {
            if (entry.allocator)
a                 entry.allocator->Reset();
            entry.in_use.store(false, std::memory_order_release);
        }
    }

    // Acquire a command list for recording. Returns nullptr if pool is exhausted.
    // Thread-safe: uses compare-exchange to prevent two threads acquiring the same entry.
    // The returned command list is already open (Reset + in recording state).
    // The caller must set root signature, descriptor heaps, etc. before recording.
    Entry* acquire(ID3D12PipelineState* initialPSO = nullptr)
    {
        for (auto& entry : m_entries)
        {
            bool expected = false;
            if (entry.in_use.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                entry.cmdList->Reset(entry.allocator.Get(), initialPSO);
                return &entry;
            }
        }
        return nullptr; // Pool exhausted
    }

    // Get all currently in-use command lists (for ExecuteCommandLists).
    std::vector<ID3D12CommandList*> getActiveCommandLists() const
    {
        std::vector<ID3D12CommandList*> result;
        for (const auto& entry : m_entries)
        {
            if (entry.in_use.load(std::memory_order_acquire))
                result.push_back(entry.cmdList.Get());
        }
        return result;
    }

    uint32_t capacity() const { return static_cast<uint32_t>(m_entries.size()); }
    uint32_t activeCount() const
    {
        uint32_t count = 0;
        for (const auto& entry : m_entries)
            if (entry.in_use.load(std::memory_order_acquire)) count++;
        return count;
    }

    void shutdown()
    {
        m_entries.clear();
    }

private:
    std::vector<Entry> m_entries;
};
