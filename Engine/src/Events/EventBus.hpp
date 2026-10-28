#pragma once

#include "EngineExport.h"
#include <entt/entt.hpp>
#include <functional>
#include "ECS/ComponentCatalog.hpp"

class ENGINE_API EventBus
{
public:
    static EventBus& get()
    {
        static EventBus instance;
        return instance;
    }

    // Host-thread source scope for module exports. The owner identity must
    // remain alive for the duration of the scope.
    class ENGINE_API ModuleSourceScope
    {
    public:
        explicit ModuleSourceScope(const garden::ecs::ComponentOwnerId& source) noexcept;
        ModuleSourceScope(garden::ecs::ComponentOwnerId&&) = delete;
        ~ModuleSourceScope();
        ModuleSourceScope(const ModuleSourceScope&) = delete;
        ModuleSourceScope& operator=(const ModuleSourceScope&) = delete;
    private:
        const garden::ecs::ComponentOwnerId* previous_;
    };

    // EnTT retains event pools after listener disconnection. Until those pools
    // have engine-owned lifetime, a source that created or touched one cannot
    // be safely unloaded, even when all its listeners disconnected.
    [[nodiscard]] static bool moduleSourceCanRetire(const garden::ecs::ComponentOwnerId& source) noexcept;

    // Subscribe to an event type. Returns a connection that can be used to disconnect.
    // Usage: EventBus::get().subscribe<MyEvent>([](const MyEvent& e) { ... });
    template<typename Event>
    auto sink()
    {
        markCurrentModuleUse();
        return dispatcher.sink<Event>();
    }

    // Publish an event immediately to all listeners
    template<typename Event>
    void publish(Event&& event)
    {
        markCurrentModuleUse();
        dispatcher.trigger(std::forward<Event>(event));
    }

    // Queue an event for deferred dispatch (processed during flush())
    template<typename Event>
    void queue(Event&& event)
    {
        markCurrentModuleUse();
        dispatcher.enqueue(std::forward<Event>(event));
    }

    // Queue an event constructed in-place
    template<typename Event, typename... Args>
    void emplace(Args&&... args)
    {
        markCurrentModuleUse();
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
        markCurrentModuleUse();
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
    static void markCurrentModuleUse() noexcept;
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    entt::dispatcher dispatcher;
};
