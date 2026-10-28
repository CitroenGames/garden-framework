#pragma once

#include "ECS/ComponentStore.hpp"
#include <entt/entt.hpp>

// EngineCore-private bridge. Component identity remains catalog-owned.
namespace garden::ecs::engine_adapters {
using Constructor = bool (*)(void*, void*) noexcept;
void registerComponents() noexcept;
ComponentStoreResult validate(ComponentKey, ComponentId) noexcept;
bool handles(ComponentId) noexcept;
ComponentStoreResult add(entt::registry&, ComponentId, ComponentEntityKey, Constructor, void*) noexcept;
ComponentStoreResult get(const entt::registry&, ComponentId, ComponentEntityKey, void*&) noexcept;
ComponentStoreResult remove(entt::registry&, ComponentId, ComponentEntityKey) noexcept;
ComponentStoreResult snapshot(const entt::registry&, ComponentId, std::vector<ComponentEntityKey>&) noexcept;
}
