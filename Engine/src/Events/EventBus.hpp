#pragma once

#include <entt/entt.hpp>
#include <functional>

class EventBus
{
public:
    static EventBus& get()
    {
        static EventBus instance;
        return instance;
    }

    // Subscribe to an event type. Returns a connection that can be used to disconnect.
    // Usage: EventBus::get().subscribe<MyEvent>([](const MyEvent& e) { ... });
    template<typename Event>
    auto sink()
    {
        return dispatcher.sink<Event>();
    }

    // Publish an event immediately to all listeners
    template<typename Event>
    void publish(Event&& event)
    {
        dispatcher.trigger(std::forward<Event>(event));
    }

    // Queue an event for deferred dispatch (processed during flush())
    template<typename Event>
    void queue(Event&& event)
    {
        dispatcher.enqueue(std::forward<Event>(event));
    }

    // Queue an event constructed in-place
    template<typename Event, typename... Args>
    void emplace(Args&&... args)
    {
        dispatcher.enqueue<Event>(std::forward<Args>(args)...);
    }

    // Flush all queued events - call once per frame
    void flush()
    {
        dispatcher.update();
    }

    // Flush only events of a specific type
    template<typename Event>
    void flush()
    {
        dispatcher.update<Event>();
    }

    // Disconnect all listeners for a specific instance
    template<typename Type>
    void disconnect(Type* instance)
    {
        dispatcher.disconnect(instance);
    }

    // Discard all queued events without dispatching
    void clear()
    {
        dispatcher.clear();
    }

private:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    entt::dispatcher dispatcher;
};
