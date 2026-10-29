#include "Events/EventBus.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

namespace {
thread_local const garden::ecs::ComponentOwnerId* current_module_source = nullptr;
std::mutex module_use_mutex;
std::vector<garden::ecs::ComponentOwnerId> module_uses;
std::atomic<bool> tracking_failed{false};
}

EventBus::ModuleSourceScope::ModuleSourceScope(const garden::ecs::ComponentOwnerId& source) noexcept
    : previous_(current_module_source)
{
    current_module_source = &source;
}

EventBus::ModuleSourceScope::~ModuleSourceScope()
{
    current_module_source = previous_;
}

void EventBus::flushCreated()
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

void EventBus::markCurrentModuleUse() noexcept
{
    if (!current_module_source) return;
    try {
        std::lock_guard lock(module_use_mutex);
        if (current_module_source->kind != garden::ecs::ComponentOwnerKind::Module) {
            tracking_failed.store(true, std::memory_order_release);
            return;
        }
        if (std::find(module_uses.begin(), module_uses.end(), *current_module_source) == module_uses.end())
            module_uses.push_back(*current_module_source);
    } catch (...) {
        tracking_failed.store(true, std::memory_order_release);
    }
}

bool EventBus::moduleSourceCanRetire(const garden::ecs::ComponentOwnerId& source) noexcept
{
    if (source.kind != garden::ecs::ComponentOwnerKind::Module || tracking_failed.load(std::memory_order_acquire)) return false;
    try {
        std::lock_guard lock(module_use_mutex);
        return !tracking_failed.load(std::memory_order_acquire) && std::find(module_uses.begin(), module_uses.end(), source) == module_uses.end();
    } catch (...) {
        return false;
    }
}
