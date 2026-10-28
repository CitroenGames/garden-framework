#pragma once
#include "EngineExport.h"
#include "Audio/AudioSystem.hpp"
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

struct VoiceBudget
{
    int max_voices = 64;
    int max_per_event = 8;

    // Zero is an explicit hard cap: it admits no event voices. Negative
    // values are invalid configuration, never an alias for "unlimited".
    bool isValid() const { return max_voices >= 0 && max_per_event >= 0; }
};

// Centralized counters make a measured production budget possible without
// putting mixer ownership or transient one-shots into ECS.
struct VoiceBudgetStats
{
    uint64_t admitted = 0;
    uint64_t dropped = 0;
    uint64_t stolen = 0;
    uint64_t reaped = 0;
    size_t peak_active = 0;
};

struct VoiceCandidate
{
    SoundHandle handle = INVALID_SOUND;
    std::string event_name;
    int priority = 50;
    float gain = 1.0f;       // distance/effective gain at play time, 0..1+
    uint64_t sequence = 0;   // monotonic start order; lower = older
    bool loop = false;
};

// Pure steal selection: lowest priority, then quietest (lowest gain),
// then oldest (lowest sequence). Loops are never stolen by one-shots.
// Returns index into candidates, or nullopt when nothing may be stolen.
inline std::optional<size_t> selectVoiceToSteal(const std::vector<VoiceCandidate>& candidates,
                                                bool incoming_loop = false)
{
    std::optional<size_t> best;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& c = candidates[i];
        if (c.handle == INVALID_SOUND) continue;
        if (!incoming_loop && c.loop) continue;
        if (!best) { best = i; continue; }
        const auto& b = candidates[*best];
        if (c.priority < b.priority) { best = i; continue; }
        if (c.priority > b.priority) continue;
        if (c.gain < b.gain) { best = i; continue; }
        if (c.gain > b.gain) continue;
        if (c.sequence < b.sequence) best = i;
    }
    return best;
}

// Random clip pick with no immediate repeat. random_value is any uint32
// from the caller's RNG; result is deterministic for a given value.
inline int pickClipIndexNoRepeat(int clip_count, int last_index, uint32_t random_value)
{
    if (clip_count <= 1) return 0;
    if (last_index < 0 || last_index >= clip_count)
        return static_cast<int>(random_value % static_cast<uint32_t>(clip_count));
    uint32_t r = random_value % static_cast<uint32_t>(clip_count - 1);
    int idx = static_cast<int>(r);
    if (idx >= last_index) ++idx;
    return idx;
}

inline bool isFiniteVec3(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
