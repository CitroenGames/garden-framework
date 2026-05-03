#pragma once

#include "D3D12BarrierBatch.hpp"
#include <d3d12.h>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

// Pending resource transition request (recorded during parallel command building).
// Resolved against global state during submission on the main thread.
struct PendingTransition
{
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES desiredState;
};

// Automatically tracks resource states and batches transitions.
// Thread-safe via shared_mutex: concurrent reads during parallel recording,
// exclusive writes during submission. Supports split barriers for GPU scheduling overlap.
class D3D12ResourceStateTracker
{
public:
    // Register a resource with its initial state (main thread only).
    void track(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);

    // Unregister a resource (main thread only).
    void untrack(ID3D12Resource* resource);

    // Request a transition. Adds to the internal barrier batch.
    // No-op if the resource is already in the requested state.
    // Thread-safe: acquires exclusive lock.
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);

    // Request a transition if the resource is tracked. Returns false when the
    // resource is unknown so the caller can use its explicit fallback path.
    bool transitionIfTracked(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);

    // Begin a split barrier (BEGIN_ONLY). The GPU can overlap the transition
    // with subsequent work that doesn't touch this resource.
    // Emitted immediately (not batched) since timing matters.
    // Main thread only.
    void beginSplit(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState,
                    ID3D12GraphicsCommandList* cmdList);

    // End a split barrier (END_ONLY). Completes the transition.
    // Main thread only.
    void endSplit(ID3D12Resource* resource, ID3D12GraphicsCommandList* cmdList);

    // Flush all pending batched barriers to the command list (main thread only).
    void flush(ID3D12GraphicsCommandList* cmdList);

    // Query current tracked state (thread-safe, shared lock).
    D3D12_RESOURCE_STATES getState(ID3D12Resource* resource) const;

    // Check if a resource is tracked (thread-safe, shared lock).
    bool isTracked(ID3D12Resource* resource) const;

    // Resolve a list of pending transitions against the global state map.
    // Emits actual barriers to the command list and updates tracked state.
    // Call on main thread during submission.
    void resolvePendingTransitions(const std::vector<PendingTransition>& pending,
                                   ID3D12GraphicsCommandList* cmdList);

private:
    struct ResourceState
    {
        D3D12_RESOURCE_STATES current;
        D3D12_RESOURCE_STATES splitTarget; // Target state during a split barrier
        bool inSplit = false;
    };

    std::unordered_map<ID3D12Resource*, ResourceState> m_states;
    BarrierBatch m_batch;
    mutable std::shared_mutex m_mutex; // Protects m_states
};
