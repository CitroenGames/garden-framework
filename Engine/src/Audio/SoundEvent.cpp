#include "Audio/SoundEvent.hpp"
#include "Components/Components.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <cmath>

namespace
{
SoundHandle defaultPlay(const SoundEventDef& def, const std::string& clip_path,
                        const glm::vec3& position, bool spatial, float volume, float pitch)
{
    auto& audio = AudioSystem::get();
    if (!audio.isInitialized()) return INVALID_SOUND;
    AudioClipId clip = audio.loadClip(clip_path);
    if (clip == INVALID_AUDIO_CLIP) return INVALID_SOUND;
    if (spatial)
    {
        Play3DParams p; p.volume = volume; p.pitch = pitch; p.loop = def.loop; p.group = def.group;
        p.min_distance = def.min_distance; p.max_distance = def.max_distance;
        return audio.playSound3D(clip, position, p);
    }
    PlayParams p; p.volume = volume; p.pitch = pitch; p.loop = def.loop; p.group = def.group;
    return audio.playSound(clip, p);
}
void defaultStop(SoundHandle h) { AudioSystem::get().stopSound(h); }
}

void AudioEventSystem::registerEvent(SoundEventDef def)
{
    if (def.name.empty()) return;
    std::string key = def.name;
    m_events[key] = std::move(def);
}
void AudioEventSystem::clearEvents() { m_events.clear(); m_last_clip.clear(); }
const SoundEventDef* AudioEventSystem::findEvent(const std::string& name) const
{
    auto it = m_events.find(name);
    return it == m_events.end() ? nullptr : &it->second;
}
void AudioEventSystem::resetBackendToDefault() { m_play_fn = nullptr; m_stop_fn = nullptr; }

bool AudioEventSystem::shouldLogFailure(const std::string& event_name)
{
    auto now = std::chrono::steady_clock::now();
    auto it = m_last_log.find(event_name);
    if (it != m_last_log.end() && now - it->second < std::chrono::seconds(1)) return false;
    m_last_log[event_name] = now;
    ++m_log_count;
    return true;
}

float AudioEventSystem::distanceGain(const SoundEventDef& def, const glm::vec3& position, bool spatial) const
{
    if (!spatial || !m_has_listener) return 1.0f;
    float d = glm::distance(m_listener, position);
    if (d <= def.min_distance) return 1.0f;
    if (def.max_distance <= def.min_distance) return 0.0f;
    return std::clamp(1.0f - (d - def.min_distance) / (def.max_distance - def.min_distance), 0.0f, 1.0f);
}

size_t AudioEventSystem::activeVoiceCountForEvent(const std::string& name) const
{
    size_t n = 0;
    for (auto& v : m_voices) if (v.event_name == name) ++n;
    return n;
}
int AudioEventSystem::lastClipIndex(const std::string& name) const
{
    auto it = m_last_clip.find(name);
    return it == m_last_clip.end() ? -1 : it->second;
}

bool AudioEventSystem::enforceCaps(const SoundEventDef& def, float gain)
{
    auto stop_voice = [&](size_t idx) {
        SoundHandle h = m_voices[idx].handle;
        if (m_stop_fn) m_stop_fn(h); else defaultStop(h);
        m_voices.erase(m_voices.begin() + static_cast<long>(idx));
    };
    // Per-event cap: steal from the same event (same priority -> quietest/oldest).
    int per_event_cap = std::min(def.max_voices, m_budget.max_per_event);
    if (per_event_cap >= 0)
    {
        while (static_cast<int>(activeVoiceCountForEvent(def.name)) >= per_event_cap && per_event_cap > 0)
        {
            std::vector<VoiceCandidate> cands;
            std::vector<size_t> idxs;
            for (size_t i = 0; i < m_voices.size(); ++i)
                if (m_voices[i].event_name == def.name)
                { cands.push_back({m_voices[i].handle, m_voices[i].event_name, m_voices[i].priority, m_voices[i].gain, m_voices[i].sequence, m_voices[i].loop}); idxs.push_back(i); }
            auto pick = selectVoiceToSteal(cands, def.loop);
            if (!pick) return false;
            stop_voice(idxs[*pick]);
        }
        if (per_event_cap == 0) return false;
    }
    // Global budget.
    while (m_budget.max_voices > 0 && static_cast<int>(m_voices.size()) >= m_budget.max_voices)
    {
        std::vector<VoiceCandidate> cands;
        for (auto& v : m_voices) cands.push_back({v.handle, v.event_name, v.priority, v.gain, v.sequence, v.loop});
        auto pick = selectVoiceToSteal(cands, def.loop);
        if (!pick) return false;
        // Only steal a strictly lower-or-equal priority voice.
        if (cands[*pick].priority > def.priority) return false;
        stop_voice(*pick);
    }
    (void)gain;
    return true;
}

SoundHandle AudioEventSystem::playInternal(const SoundEventDef& def, const glm::vec3& position, bool spatial)
{
    if (spatial && !isFiniteVec3(position)) return INVALID_SOUND;
    if (def.clip_paths.empty())
    {
        if (shouldLogFailure(def.name)) LOG_ENGINE_WARN("Audio event '{}' has no clips", def.name);
        return INVALID_SOUND;
    }
    // Relevance cull: beyond max_distance*1.2, skip (not just silent).
    if (spatial && m_has_listener && def.max_distance > 0.0f)
    {
        if (glm::distance(m_listener, position) > def.max_distance * 1.2f) return INVALID_SOUND;
    }
    float gain = distanceGain(def, position, spatial);
    if (!enforceCaps(def, gain)) return INVALID_SOUND;

    int last = lastClipIndex(def.name);
    int idx = pickClipIndexNoRepeat(static_cast<int>(def.clip_paths.size()), last,
                                    static_cast<uint32_t>(m_rng()));
    float pitch = def.pitch_min;
    if (def.pitch_max > def.pitch_min)
    {
        float t = static_cast<float>(m_rng() % 10000) / 10000.0f;
        pitch = def.pitch_min + t * (def.pitch_max - def.pitch_min);
    }
    SoundHandle handle = INVALID_SOUND;
    if (m_play_fn) handle = m_play_fn(def, def.clip_paths[static_cast<size_t>(idx)], position, spatial, def.volume, pitch);
    else handle = defaultPlay(def, def.clip_paths[static_cast<size_t>(idx)], position, spatial, def.volume, pitch);
    if (handle == INVALID_SOUND)
    {
        if (shouldLogFailure(def.name)) LOG_ENGINE_WARN("Audio event '{}' failed to start a voice (missing clip or audio unavailable)", def.name);
        return INVALID_SOUND;
    }
    m_last_clip[def.name] = idx;
    m_voices.push_back({handle, def.name, def.priority, gain, m_next_sequence++, def.loop});
    return handle;
}

SoundHandle AudioEventSystem::playAt(const std::string& event_name, const glm::vec3& position)
{
    const SoundEventDef* def = findEvent(event_name);
    if (!def)
    {
        if (shouldLogFailure(event_name)) LOG_ENGINE_WARN("Unknown audio event '{}'", event_name);
        return INVALID_SOUND;
    }
    // Copy def: registerEvent during play callbacks could invalidate ref.
    SoundEventDef copy = *def;
    return playInternal(copy, position, true);
}
SoundHandle AudioEventSystem::play2D(const std::string& event_name)
{
    const SoundEventDef* def = findEvent(event_name);
    if (!def)
    {
        if (shouldLogFailure(event_name)) LOG_ENGINE_WARN("Unknown audio event '{}'", event_name);
        return INVALID_SOUND;
    }
    SoundEventDef copy = *def;
    return playInternal(copy, glm::vec3(0.0f), false);
}
SoundHandle AudioEventSystem::play(entt::registry& registry, const std::string& event_name, entt::entity at_entity)
{
    if (!registry.valid(at_entity)) return play2D(event_name);
    glm::vec3 pos(0.0f);
    if (auto* t = registry.try_get<TransformComponent>(at_entity)) pos = t->position;
    else return play2D(event_name);
    return playAt(event_name, pos);
}
void AudioEventSystem::stopAll()
{
    for (auto& v : m_voices) { if (m_stop_fn) m_stop_fn(v.handle); else defaultStop(v.handle); }
    m_voices.clear();
}
void AudioEventSystem::reapFinished(const std::function<bool(SoundHandle)>& is_playing)
{
    if (!is_playing) return;
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
        [&](const TrackedVoice& v) { return !v.loop && !is_playing(v.handle); }), m_voices.end());
}
