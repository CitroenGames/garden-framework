#pragma once

#include "ECS/ComponentCatalog.hpp"
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace garden::ecs {

// Each component owner specializes this trait in the shared type declaration.
// As with catalog registration, matching keys cannot prove C++ ABI compatibility.
template<class T> struct ComponentType;

// A world owns the meaning and validity of these keys. Slot reuse must first
// remove all values for the previous generation.
struct ComponentEntityKey {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
};

enum class ComponentStoreResult {
    Success, InvalidEntity, StaleEntity, StaleComponent, Duplicate, NotFound, UnregisteredType,
    TypeMismatch, ConstructionFailed, ResourceExhausted, InternalError,
    StructuralMutationDuringQuery, CallbackFailed
};

// Externally synchronized. Returned pointers remain stable until removal or
// destruction of the store. This does not validate world ownership or liveness.
class ENGINE_API ComponentStore {
public:
    ComponentStore();
    ~ComponentStore();
    ComponentStore(const ComponentStore&) = delete;
    ComponentStore& operator=(const ComponentStore&) = delete;

    template<class T, class... Args>
    ComponentStoreResult add(ComponentId id, ComponentEntityKey entity, Args&&... args) {
        static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>, "Components require a non-throwing destructor");
        auto arguments = std::forward_as_tuple(std::forward<Args>(args)...);
        auto construct = [](void* destination, void* context) noexcept -> bool {
            try {
                auto& tuple = *static_cast<decltype(arguments)*>(context);
                std::apply([destination](auto&&... values) {
                    ::new (destination) T(std::forward<decltype(values)>(values)...);
                }, std::move(tuple));
                return true;
            } catch (...) { return false; }
        };
        return addErased(id, ComponentType<T>::key, entity, sizeof(T), alignof(T), construct, &arguments);
    }

    ComponentStoreResult get(ComponentId, ComponentEntityKey, void*& out) const noexcept;
    // Copies the keys present for a registered component, in unspecified order.
    // Empty pools succeed; errors clear out. The caller must synchronize structural
    // changes and validate keys against the world before using them.
    ComponentStoreResult snapshotEntities(ComponentId, std::vector<ComponentEntityKey>& out) const noexcept;
    ComponentStoreResult remove(ComponentId, ComponentEntityKey) noexcept;
    // Called by the world before recycling an entity slot.
    ComponentStoreResult removeEntity(ComponentEntityKey) noexcept;
    void clear() noexcept;

private:
    using Constructor = bool (*)(void*, void*) noexcept;
    ComponentStoreResult addErased(ComponentId, ComponentKey, ComponentEntityKey, size_t, size_t,
        Constructor, void*) noexcept;
    struct Impl;
    Impl* m_impl;
};

} // namespace garden::ecs
