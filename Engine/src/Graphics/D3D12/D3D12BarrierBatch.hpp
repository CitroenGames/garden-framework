#pragma once

#include <d3d12.h>
#include <vector>
#include <cstdint>

// Accumulates resource barriers and submits them in a single ResourceBarrier() call.
// This reduces GPU command processor stalls compared to issuing barriers one at a time.
class BarrierBatch
{
public:
    static constexpr uint32_t MAX_BARRIERS = 16;

    BarrierBatch() { m_barriers.reserve(MAX_BARRIERS); }

    void add(ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after,
             UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        if (before == after) return;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = subresource;
        m_barriers.push_back(barrier);
    }

    void flush(ID3D12GraphicsCommandList* cmdList)
    {
        if (m_barriers.empty()) return;
        cmdList->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
        m_barriers.clear();
    }

    uint32_t count() const { return static_cast<uint32_t>(m_barriers.size()); }

private:
    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
};
