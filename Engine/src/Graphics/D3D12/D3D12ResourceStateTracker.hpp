#pragma once

#include "D3D12BarrierBatch.hpp"
#include <d3d12.h>
#include <unordered_map>

// Automatically tracks resource states and batches transitions.
// Replaces manual per-resource state variables with a centralized tracker.
// Supports split barriers for GPU scheduling overlap.
class D3D12ResourceStateTracker
{
public:
    // Register a resource with its initial state.
    void track(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);

    // Unregister a resource (e.g., on destruction).
    void untrack(ID3D12Resource* resource);

    // Request a transition. Adds to the internal barrier batch.
    // No-op if the resource is already in the requested state.
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);

    // Begin a split barrier (BEGIN_ONLY). The GPU can overlap the transition
    // with subsequent work that doesn't touch this resource.
    // Emitted immediately (not batched) since timing matters.
    void beginSplit(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState,
                    ID3D12GraphicsCommandList* cmdList);

    // End a split barrier (END_ONLY). Completes the transition.
    void endSplit(ID3D12Resource* resource, ID3D12GraphicsCommandList* cmdList);

    // Flush all pending batched barriers to the command list.
    void flush(ID3D12GraphicsCommandList* cmdList);

    // Query current tracked state.
    D3D12_RESOURCE_STATES getState(ID3D12Resource* resource) const;

    bool isTracked(ID3D12Resource* resource) const;

private:
    struct ResourceState
    {
        D3D12_RESOURCE_STATES current;
        D3D12_RESOURCE_STATES splitTarget; // Target state during a split barrier
        bool inSplit = false;
    };

    std::unordered_map<ID3D12Resource*, ResourceState> m_states;
    BarrierBatch m_batch;
};
