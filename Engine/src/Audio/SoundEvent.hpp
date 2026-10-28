#pragma once
#include "EngineExport.h"
#include "Audio/AudioSystem.hpp"
#include "Audio/VoiceBudget.hpp"
#include <chrono>
#include <entt/entt.hpp>
#include <functional>
#include <glm/glm.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class SoundEventTable;

// Data-driven sound event definition. Event names and clip paths are
// owned by game/template data; the engine ships no event names.
struct SoundEventDef
{
    std::string name;
    std::vector<std::string> clip_paths; // resolved via AudioSystem::loadClip
    float volume = 1.0f;
    float pitch_min = 0.95f;
    float pitch_max = 1.05f;
    AudioGroup group = AudioGroup::SFX;
    float min_distance = 1.0f;
    float max_distance = 40.0f;
    int priority = 50;   // 0..100
    int max_voices = 4;  // per-event cap (further clamped by VoiceBudget)
    bool loop = false;
};

class ENGINE_API AudioEventSystem
{
public:
    // Backend hooks; default implementation drives AudioSystem.
    // Tests inject fakes so headless runs never open a device.
    using PlayFn = std::function<SoundHandle(const SoundEventDef& def, const std::string& clip_path,
                                             const glm::vec3& position, bool spatial,
                                             float volume, float pitch)>;
    using StopFn = std::function<void(SoundHandle)>;

    static AudioEventSystem& get()
    {
        static AudioEventSystem instance;
        return instance;
    }

    // Incremental registration is for code-owned template data. Duplicates and
    // invalid definitions reject without changing the current table.
    bool registerEvent(SoundEventDef def, std::string* error = nullptr);
    // Replace all definitions as one presentation-frame operation. Existing
    // active voices retain their mixer ownership until naturally reaped or
    // stopped; only future event lookups use the replacement table.
    bool replaceEventTable(const SoundEventTable& table, std::string* error = nullptr);
    void clearEvents();
    const SoundEventDef* findEvent(const std::string& name) const;
    size_t eventCount() const { return m_events.size(); }

    // Reject invalid configuration without changing the active budget. A zero
    // cap is valid and explicitly disables admission at that scope.
    bool setBudget(const VoiceBudget& budget);
    const VoiceBudget& budget() const { return m_budget; }
    const VoiceBudgetStats& budgetStats() const { return m_budget_stats; }
    void resetBudgetStats() { m_budget_stats = {}; }
    void setPlayFn(PlayFn fn) { m_play_fn = std::move(fn); }
    void setStopFn(StopFn fn) { m_stop_fn = std::move(fn); }
    void resetBackendToDefault();
    void setListenerPosition(const glm::vec3& pos) { m_listener = pos; m_has_listener = true; }
    void clearListener() { m_has_listener = false; }
    void setSeed(uint32_t seed) { m_rng.seed(seed); }

    SoundHandle playAt(const std::string& event_name, const glm::vec3& position);
    SoundHandle play2D(const std::string& event_name);
    SoundHandle play(entt::registry& registry, const std::string& event_name, entt::entity at_entity);

    void stopAll();
    void reapFinished(const std::function<bool(SoundHandle)>& is_playing);
    size_t activeVoiceCount() const { return m_voices.size(); }
    size_t activeVoiceCountForEvent(const std::string& name) const;
    int lastClipIndex(const std::string& name) const;
    uint64_t diagnosticLogCount() const { return m_log_count; }
    // Rate-limit gate (1/s per name); exposed for tests. Counts a log when allowed.
    bool shouldLogFailure(const std::string& event_name);

private:
    SoundHandle playInternal(const SoundEventDef& def, const glm::vec3& position, bool spatial);
    float distanceGain(const SoundEventDef& def, const glm::vec3& position, bool spatial) const;
    bool enforceCaps(const SoundEventDef& def, float gain);

    std::unordered_map<std::string, SoundEventDef> m_events;
    std::unordered_map<std::string, int> m_last_clip;
    struct TrackedVoice { SoundHandle handle; std::string event_name; int priority; float gain; uint64_t sequence; bool loop; };
    std::vector<TrackedVoice> m_voices;
    VoiceBudget m_budget;
    VoiceBudgetStats m_budget_stats;
    PlayFn m_play_fn;
    StopFn m_stop_fn;
    glm::vec3 m_listener{0.0f};
    bool m_has_listener = false;
    std::mt19937 m_rng{std::random_device{}()};
    uint64_t m_next_sequence = 1;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_last_log;
    uint64_t m_log_count = 0;
};

namespace AudioEvents
{
    inline SoundHandle playAt(const std::string& e, const glm::vec3& p) { return AudioEventSystem::get().playAt(e, p); }
    inline SoundHandle play2D(const std::string& e) { return AudioEventSystem::get().play2D(e); }
    inline SoundHandle play(entt::registry& r, const std::string& e, entt::entity at) { return AudioEventSystem::get().play(r, e, at); }
}
