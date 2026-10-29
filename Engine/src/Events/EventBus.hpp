#pragma once

#include "EngineExport.h"
#include <entt/entt.hpp>
#include <cstdint>
#include <functional>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include "ECS/ComponentCatalog.hpp"
#include "Events/EngineEvents.hpp"

struct EventBusTestAccess;

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
        if constexpr (std::is_same_v<std::remove_cvref_t<Event>, EntityCreatedEvent>)
            createdEvents.emplace_back(std::forward<Event>(event));
        else
            dispatcher.enqueue(std::forward<Event>(event));
    }

    // Queue an event constructed in-place
    template<typename Event, typename... Args>
    void emplace(Args&&... args)
    {
        markCurrentModuleUse();
        if constexpr (std::is_same_v<Event, EntityCreatedEvent>)
            createdEvents.emplace_back(std::forward<Args>(args)...);
        else
            dispatcher.enqueue<Event>(std::forward<Args>(args)...);
    }

    // Reserves before moving any payload. On failure both queues are unchanged.
    [[nodiscard]] bool queueEntityCreatedBatch(std::vector<EntityCreatedEvent>&& events) noexcept
    {
        markCurrentModuleUse();
        static_assert(std::is_nothrow_move_constructible_v<EntityCreatedEvent>);
        if (events.size() > createdEvents.max_size() - createdEvents.size()) return false;
        try {
            if (failNextCreatedBatchAllocation) {
                failNextCreatedBatchAllocation = false;
                throw std::bad_alloc{};
            }
            createdEvents.reserve(createdEvents.size() + events.size());
        } catch (...) {
            return false;
        }
        for (auto& event : events) createdEvents.emplace_back(std::move(event));
        events.clear();
        return true;
    }

    // Flush all queued events - call once per frame
    void flush()
    {
        FlushGuard guard(*this);
        flushCreated();
        dispatcher.update();
    }

    // Flush only events of a specific type
    template<typename Event>
    void flush()
    {
        markCurrentModuleUse();
        FlushGuard guard(*this);
        if constexpr (std::is_same_v<Event, EntityCreatedEvent>) flushCreated();
        else dispatcher.update<Event>();
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
        ++clearGeneration;
        createdEvents.clear();
        dispatcher.clear();
    }

private:
    friend struct EventBusTestAccess;
    class FlushGuard
    {
    public:
        explicit FlushGuard(EventBus& bus) : bus_(bus) {
            if (bus_.flushing) throw std::logic_error("reentrant EventBus flush");
            bus_.flushing = true;
        }
        ~FlushGuard() { bus_.flushing = false; }
    private:
        EventBus& bus_;
    };

    void flushCreated()
    {
        const auto generation = clearGeneration;
        size_t remaining = createdEvents.size();
        while (remaining && !createdEvents.empty() && generation == clearGeneration) {
            EntityCreatedEvent event = std::move(createdEvents.front());
            createdEvents.erase(createdEvents.begin());
            --remaining;
            dispatcher.trigger(std::move(event));
        }
    }

    static void markCurrentModuleUse() noexcept;
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    entt::dispatcher dispatcher;
    std::vector<EntityCreatedEvent> createdEvents;
    uint64_t clearGeneration = 0;
    bool flushing = false;
    bool failNextCreatedBatchAllocation = false;
};
