#include "D3D12ResourceStateTracker.hpp"

void D3D12ResourceStateTracker::track(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState)
{
    if (!resource) return;
    std::unique_lock lock(m_mutex);
    m_states[resource] = { initialState, initialState, false };
}

void D3D12ResourceStateTracker::untrack(ID3D12Resource* resource)
{
    std::unique_lock lock(m_mutex);
    m_states.erase(resource);
}

void D3D12ResourceStateTracker::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState)
{
    if (!resource) return;
    std::unique_lock lock(m_mutex);
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
    std::unique_lock lock(m_mutex);
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
    // Don't update current yet -- it changes when endSplit is called
}

void D3D12ResourceStateTracker::endSplit(ID3D12Resource* resource, ID3D12GraphicsCommandList* cmdList)
{
    if (!resource || !cmdList) return;
    std::unique_lock lock(m_mutex);
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
    // transition() pushes into m_batch under the exclusive lock, so flush must
    // take the same lock to avoid a data race if any worker thread is still
    // recording transitions when the main thread flushes.
    std::unique_lock lock(m_mutex);
    m_batch.flush(cmdList);
}

D3D12_RESOURCE_STATES D3D12ResourceStateTracker::getState(ID3D12Resource* resource) const
{
    std::shared_lock lock(m_mutex);
    auto it = m_states.find(resource);
    if (it != m_states.end())
        return it->second.current;
    return D3D12_RESOURCE_STATE_COMMON;
}

bool D3D12ResourceStateTracker::isTracked(ID3D12Resource* resource) const
{
    std::shared_lock lock(m_mutex);
    return m_states.find(resource) != m_states.end();
}

void D3D12ResourceStateTracker::resolvePendingTransitions(const std::vector<PendingTransition>& pending,
                                                           ID3D12GraphicsCommandList* cmdList)
{
    if (pending.empty() || !cmdList) return;

    std::unique_lock lock(m_mutex);
    for (const auto& pt : pending)
    {
        auto it = m_states.find(pt.resource);
        if (it == m_states.end()) continue;

        auto& state = it->second;
        if (state.current == pt.desiredState) continue;

        m_batch.add(pt.resource, state.current, pt.desiredState);
        state.current = pt.desiredState;
    }

    m_batch.flush(cmdList);
}
