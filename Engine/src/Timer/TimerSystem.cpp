#include "TimerSystem.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"
#include <algorithm>

TimerId TimerSystem::createTimer(float duration, TimerCallback callback, bool repeating)
{
    TimerId id = next_id++;
    timers.push_back({id, duration, 0.0f, std::move(callback), repeating, false, true, true});
    return id;
}

TimerId TimerSystem::createDelay(float delay, std::function<void()> callback)
{
    return createTimer(delay, [cb = std::move(callback)](TimerId) { cb(); }, false);
}

void TimerSystem::cancelTimer(TimerId id)
{
    if (auto* timer = findTimer(id))
    {
        timer->active = false;
    }
}

void TimerSystem::pauseTimer(TimerId id)
{
    if (auto* timer = findTimer(id))
    {
        timer->paused = true;
    }
}

void TimerSystem::resumeTimer(TimerId id)
{
    if (auto* timer = findTimer(id))
    {
        timer->paused = false;
    }
}

float TimerSystem::getRemaining(TimerId id) const
{
    if (const auto* timer = findTimer(id))
    {
        return timer->duration - timer->elapsed;
    }
    return 0.0f;
}

float TimerSystem::getElapsed(TimerId id) const
{
    if (const auto* timer = findTimer(id))
    {
        return timer->elapsed;
    }
    return 0.0f;
}

bool TimerSystem::isActive(TimerId id) const
{
    if (const auto* timer = findTimer(id))
    {
        return timer->active;
    }
    return false;
}

void TimerSystem::update(float delta_time)
{
    float scaled_dt = delta_time * time_scale;

    for (auto& timer : timers)
    {
        if (!timer.active || timer.paused)
            continue;

        float dt = timer.scale_affected ? scaled_dt : delta_time;
        timer.elapsed += dt;

        if (timer.elapsed >= timer.duration)
        {
            if (timer.callback)
            {
                timer.callback(timer.id);
            }

            EventBus::get().queue(TimerExpiredEvent{timer.id, timer.repeating});

            if (timer.repeating)
            {
                timer.elapsed -= timer.duration;
            }
            else
            {
                timer.active = false;
            }
        }
    }

    // Remove dead timers periodically to avoid unbounded growth
    timers.erase(
        std::remove_if(timers.begin(), timers.end(),
            [](const TimerEntry& t) { return !t.active; }),
        timers.end());
}

void TimerSystem::clear()
{
    timers.clear();
}

size_t TimerSystem::getActiveCount() const
{
    size_t count = 0;
    for (const auto& t : timers)
    {
        if (t.active) count++;
    }
    return count;
}

TimerSystem::TimerEntry* TimerSystem::findTimer(TimerId id)
{
    for (auto& t : timers)
    {
        if (t.id == id && t.active) return &t;
    }
    return nullptr;
}

const TimerSystem::TimerEntry* TimerSystem::findTimer(TimerId id) const
{
    for (const auto& t : timers)
    {
        if (t.id == id && t.active) return &t;
    }
    return nullptr;
}
