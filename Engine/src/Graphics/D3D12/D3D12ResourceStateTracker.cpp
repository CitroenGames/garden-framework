#include "D3D12ResourceStateTracker.hpp"

void D3D12ResourceStateTracker::track(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState)
{
    if (!resource) return;
    m_states[resource] = { initialState, initialState, false };
}

void D3D12ResourceStateTracker::untrack(ID3D12Resource* resource)
{
    m_states.erase(resource);
}

void D3D12ResourceStateTracker::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState)
{
    if (!resource) return;
    auto it = m_states.find(resource);
    if (it == m_states.end()) return;

    auto& state = it->second;
    if (state.current == newState) return;

    m_batch.add(resource, state.current, newState);
    state.current = newState;
}

void D3D12ResourceStateTracker::beginSplit(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState,
                                            ID3D12GraphicsCommandList* cmdList)
{
    if (!resource || !cmdList) return;
    auto it = m_states.find(resource);
    if (it == m_states.end()) return;

    auto& state = it->second;
    if (state.current == newState) return;

    // Emit BEGIN_ONLY barrier immediately (timing-sensitive, not batched)
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = state.current;
    barrier.Transition.StateAfter = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    state.splitTarget = newState;
    state.inSplit = true;
    // Don't update current yet — it changes when endSplit is called
}

void D3D12ResourceStateTracker::endSplit(ID3D12Resource* resource, ID3D12GraphicsCommandList* cmdList)
{
    if (!resource || !cmdList) return;
    auto it = m_states.find(resource);
    if (it == m_states.end()) return;

    auto& state = it->second;
    if (!state.inSplit) return;

    // Emit END_ONLY barrier to complete the split transition
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = state.current;
    barrier.Transition.StateAfter = state.splitTarget;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    state.current = state.splitTarget;
    state.inSplit = false;
}

void D3D12ResourceStateTracker::flush(ID3D12GraphicsCommandList* cmdList)
{
    m_batch.flush(cmdList);
}

D3D12_RESOURCE_STATES D3D12ResourceStateTracker::getState(ID3D12Resource* resource) const
{
    auto it = m_states.find(resource);
    if (it != m_states.end())
        return it->second.current;
    return D3D12_RESOURCE_STATE_COMMON;
}

bool D3D12ResourceStateTracker::isTracked(ID3D12Resource* resource) const
{
    return m_states.find(resource) != m_states.end();
}
