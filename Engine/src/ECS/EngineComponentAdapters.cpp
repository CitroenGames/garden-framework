#include "ECS/EngineComponentAdapters.hpp"
#include "Components/Components.hpp"
#include <array>
#include <new>
#include <utility>

namespace garden::ecs::engine_adapters {
namespace {
struct Registration {
    std::array<ComponentId, 2> ids{};
    ComponentCatalogResult result = ComponentCatalogResult::InternalError;
    Registration() noexcept {
        const std::array<ComponentDescriptor, 2> descriptors{{
            {ComponentType<TransformComponent>::key, ComponentOwnerId::engineCore(), sizeof(TransformComponent), alignof(TransformComponent), ComponentType<TransformComponent>::capabilities, makeComponentOperations<TransformComponent>()},
            {ComponentType<PlayerComponent>::key, ComponentOwnerId::engineCore(), sizeof(PlayerComponent), alignof(PlayerComponent), ComponentType<PlayerComponent>::capabilities, makeComponentOperations<PlayerComponent>()}
        }};
        result = ComponentCatalog::instance().registerBatch(ComponentOwnerId::engineCore(), descriptors, ids);
    }
};
Registration& registration() noexcept { static Registration value; return value; }
entt::entity entityOf(ComponentEntityKey key) noexcept {
    return entt::entt_traits<entt::entity>::construct(key.slot, key.generation - 1u);
}
template<class T> ComponentStoreResult addTyped(entt::registry& registry, ComponentEntityKey key, Constructor construct, void* context) noexcept {
    const auto entity = entityOf(key);
    if (!registry.valid(entity)) return ComponentStoreResult::StaleEntity;
    if (registry.all_of<T>(entity)) return ComponentStoreResult::Duplicate;
    alignas(T) unsigned char buffer[sizeof(T)];
    if (!construct(buffer, context)) return ComponentStoreResult::ConstructionFailed;
    auto* value = std::launder(reinterpret_cast<T*>(buffer));
    try {
        registry.emplace<T>(entity, std::move(*value));
        value->~T();
        return ComponentStoreResult::Success;
    } catch (...) {
        value->~T();
        return ComponentStoreResult::ResourceExhausted;
    }
}
template<class T> ComponentStoreResult getTyped(const entt::registry& registry, ComponentEntityKey key, void*& out) noexcept {
    const auto entity = entityOf(key);
    if (!registry.valid(entity)) return ComponentStoreResult::StaleEntity;
    out = const_cast<T*>(registry.try_get<T>(entity));
    return out ? ComponentStoreResult::Success : ComponentStoreResult::NotFound;
}
template<class T> ComponentStoreResult removeTyped(entt::registry& registry, ComponentEntityKey key) noexcept {
    const auto entity = entityOf(key);
    if (!registry.valid(entity)) return ComponentStoreResult::StaleEntity;
    try { return registry.remove<T>(entity) ? ComponentStoreResult::Success : ComponentStoreResult::NotFound; }
    catch (...) { return ComponentStoreResult::InternalError; }
}
template<class T> ComponentStoreResult snapshotTyped(const entt::registry& registry, std::vector<ComponentEntityKey>& out) noexcept {
    try {
        for (const auto entity : registry.view<T>())
            out.push_back({static_cast<uint32_t>(entt::to_entity(entity)), static_cast<uint32_t>(entt::to_version(entity)) + 1u});
        return ComponentStoreResult::Success;
    } catch (...) { out.clear(); return ComponentStoreResult::ResourceExhausted; }
}
}
void registerComponents() noexcept { (void)registration(); }
ComponentStoreResult validate(ComponentKey key, ComponentId id) noexcept {
    auto& r = registration();
    const int index = key == ComponentType<TransformComponent>::key ? 0 : key == ComponentType<PlayerComponent>::key ? 1 : -1;
    if (index < 0) return ComponentStoreResult::Success;
    return r.result == ComponentCatalogResult::Success && r.ids[index] == id ? ComponentStoreResult::Success : ComponentStoreResult::TypeMismatch;
}
bool handles(ComponentId id) noexcept {
    auto& r = registration();
    return r.result == ComponentCatalogResult::Success && (id == r.ids[0] || id == r.ids[1]);
}
ComponentStoreResult add(entt::registry& r, ComponentId id, ComponentEntityKey k, Constructor c, void* ctx) noexcept {
    return id == registration().ids[0] ? addTyped<TransformComponent>(r, k, c, ctx) : addTyped<PlayerComponent>(r, k, c, ctx);
}
ComponentStoreResult get(const entt::registry& r, ComponentId id, ComponentEntityKey k, void*& out) noexcept {
    out = nullptr;
    return id == registration().ids[0] ? getTyped<TransformComponent>(r, k, out) : getTyped<PlayerComponent>(r, k, out);
}
ComponentStoreResult remove(entt::registry& r, ComponentId id, ComponentEntityKey k) noexcept {
    return id == registration().ids[0] ? removeTyped<TransformComponent>(r, k) : removeTyped<PlayerComponent>(r, k);
}
ComponentStoreResult snapshot(const entt::registry& r, ComponentId id, std::vector<ComponentEntityKey>& out) noexcept {
    out.clear();
    return id == registration().ids[0] ? snapshotTyped<TransformComponent>(r, out) : snapshotTyped<PlayerComponent>(r, out);
}
}
