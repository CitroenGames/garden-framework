#include "Audio/AudioEventRouter.hpp"

#include "Audio/SoundEvent.hpp"

#include <cmath>
#include <iterator>
#include <string>

namespace
{
bool isFinitePosition(const glm::vec3& position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}
}

AudioEventRouter::AudioEventRouter(AudioEventSystem& events, size_t source_capacity)
    : m_events(events)
    , m_source_capacity(source_capacity > 0 ? source_capacity : 1)
{
}

void AudioEventRouter::makeRoomForSource()
{
    if (m_sources.size() < m_source_capacity)
        return;

    auto oldest = m_sources.begin();
    for (auto it = std::next(m_sources.begin()); it != m_sources.end(); ++it)
    {
        if (it->second.last_use < oldest->second.last_use)
            oldest = it;
    }
    m_sources.erase(oldest);
}

AudioPresentationRouteResult AudioEventRouter::route(const AudioPresentationEvent& event)
{
    if (event.source_id == 0 || event.sequence == 0 || event.event_name.empty() ||
        (event.route == AudioPresentationRoute::RemoteSpatial && !isFinitePosition(event.position)))
        return AudioPresentationRouteResult::Invalid;

    const uint64_t use = ++m_use_clock;
    auto source = m_sources.find(event.source_id);
    if (source != m_sources.end())
    {
        if (event.sequence == source->second.highest_sequence)
            return AudioPresentationRouteResult::Duplicate;
        if (event.sequence < source->second.highest_sequence)
            return AudioPresentationRouteResult::Reordered;
        source->second.highest_sequence = event.sequence;
        source->second.last_use = use;
    }
    else
    {
        makeRoomForSource();
        m_sources.emplace(event.source_id, SourceState{event.sequence, use});
    }

    const std::string event_name(event.event_name);
    if (event.route == AudioPresentationRoute::Local2D)
        m_events.play2D(event_name);
    else
        m_events.playAt(event_name, event.position);
    return AudioPresentationRouteResult::Played;
}

void AudioEventRouter::clear()
{
    m_sources.clear();
    m_use_clock = 0;
}
