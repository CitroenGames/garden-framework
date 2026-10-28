#pragma once

#include "EngineExport.h"
#include "Audio/AudioSystem.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>
#include <unordered_map>

class AudioEventSystem;

// Game code supplies opaque source identifiers, ordering and event names. The
// engine only owns duplicate suppression and the 2D versus positional mixer
// boundary; it has no knowledge of game rules, protocol types or sound names.
enum class AudioPresentationRoute : uint8_t
{
    Local2D,
    RemoteSpatial,
};

struct AudioPresentationEvent
{
    uint64_t source_id = 0;
    uint64_t sequence = 0;
    std::string_view event_name;
    glm::vec3 position{0.0f};
    AudioPresentationRoute route = AudioPresentationRoute::RemoteSpatial;
};

enum class AudioPresentationRouteResult : uint8_t
{
    Played,
    Duplicate,
    Reordered,
    Invalid,
};

class ENGINE_API AudioEventRouter
{
public:
    explicit AudioEventRouter(AudioEventSystem& events, size_t source_capacity = 256);

    // Suppresses a duplicate or an older event before it reaches the mixer.
    // A valid event is consumed even when no audio device/clip can start it,
    // so a later snapshot cannot replay historical presentation.
    AudioPresentationRouteResult route(const AudioPresentationEvent& event);
    void clear();
    size_t trackedSourceCount() const { return m_sources.size(); }

private:
    struct SourceState
    {
        uint64_t highest_sequence = 0;
        uint64_t last_use = 0;
    };

    void makeRoomForSource();

    AudioEventSystem& m_events;
    size_t m_source_capacity = 1;
    uint64_t m_use_clock = 0;
    std::unordered_map<uint64_t, SourceState> m_sources;
};
