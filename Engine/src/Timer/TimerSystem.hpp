#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>

using TimerId = uint32_t;
constexpr TimerId INVALID_TIMER = 0;

using TimerCallback = std::function<void(TimerId)>;

class TimerSystem
{
public:
    static TimerSystem& get()
    {
        static TimerSystem instance;
        return instance;
    }

    // Create a timer that fires after `duration` seconds. If repeating, it resets automatically.
    TimerId createTimer(float duration, TimerCallback callback, bool repeating = false);

    // Convenience: one-shot delayed callback
    TimerId createDelay(float delay, std::function<void()> callback);

    // Cancel a timer
    void cancelTimer(TimerId id);

    // Pause/resume a specific timer
    void pauseTimer(TimerId id);
    void resumeTimer(TimerId id);

    // Query timer state
    float getRemaining(TimerId id) const;
    float getElapsed(TimerId id) const;
    bool isActive(TimerId id) const;

    // Set global time scale (1.0 = normal, 0.5 = half speed, 0.0 = paused)
    void setTimeScale(float scale) { time_scale = scale; }
    float getTimeScale() const { return time_scale; }

    // Tick all active timers - call once per frame
    void update(float delta_time);

    // Remove all timers
    void clear();

    // Get count of active timers
    size_t getActiveCount() const;

private:
    TimerSystem() = default;
    ~TimerSystem() = default;
    TimerSystem(const TimerSystem&) = delete;
    TimerSystem& operator=(const TimerSystem&) = delete;

    struct TimerEntry
    {
        TimerId id = INVALID_TIMER;
        float duration = 0.0f;
        float elapsed = 0.0f;
        TimerCallback callback;
        bool repeating = false;
        bool paused = false;
        bool active = true;
        bool scale_affected = true; // affected by global time scale
    };

    TimerEntry* findTimer(TimerId id);
    const TimerEntry* findTimer(TimerId id) const;

    std::vector<TimerEntry> timers;
    uint32_t next_id = 1;
    float time_scale = 1.0f;
};
