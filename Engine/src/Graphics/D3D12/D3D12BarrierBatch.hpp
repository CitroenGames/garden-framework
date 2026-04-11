#pragma once

#include <d3d12.h>
#include <array>
#include <cstdint>

// Accumulates resource barriers and submits them in a single ResourceBarrier() call.
// This reduces GPU command processor stalls compared to issuing barriers one at a time.
class BarrierBatch
{
public:
    static constexpr uint32_t MAX_BARRIERS = 16;

    void add(ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after,
             UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        if (before == after) return;
        if (m_count >= MAX_BARRIERS) return; // Shouldn't happen in practice

        D3D12_RESOURCE_BARRIER& barrier = m_barriers[m_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = subresource;
    }

    void flush(ID3D12GraphicsCommandList* cmdList)
    {
        if (m_count == 0) return;
        cmdList->ResourceBarrier(m_count, m_barriers.data());
        m_count = 0;
    }

    uint32_t count() const { return m_count; }

private:
    std::array<D3D12_RESOURCE_BARRIER, MAX_BARRIERS> m_barriers = {};
    uint32_t m_count = 0;
};
