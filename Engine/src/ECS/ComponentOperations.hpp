#pragma once

#include "ECS/ComponentCatalog.hpp"
#include <new>
#include <type_traits>

namespace garden::ecs {

// Destruction has no failure channel, so a component must have a non-throwing
// destructor. Construction failures leave no object at the destination.
template<class T>
constexpr uint32_t componentCapabilities() noexcept {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>, "Components require a non-throwing destructor");
    uint32_t capabilities = 0;
    if constexpr (std::is_default_constructible_v<T>) capabilities |= ComponentDefaultConstruct;
    if constexpr (std::is_copy_constructible_v<T>) capabilities |= ComponentCopyConstruct;
    return capabilities;
}

template<class T>
constexpr ComponentOperations makeComponentOperations() noexcept {
    // Instantiate the same type checks as the capability helper.
    static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>, "Components require a non-throwing destructor");
    ComponentOperations operations{};
    operations.destroy = [](void* value) noexcept { static_cast<T*>(value)->~T(); };
    if constexpr (std::is_default_constructible_v<T>) {
        operations.defaultConstruct = [](void* destination) noexcept {
            try { ::new (destination) T(); return true; }
            catch (...) { return false; }
        };
    }
    if constexpr (std::is_copy_constructible_v<T>) {
        operations.copyConstruct = [](void* destination, const void* source) noexcept {
            try { ::new (destination) T(*static_cast<const T*>(source)); return true; }
            catch (...) { return false; }
        };
    }
    return operations;
}

} // namespace garden::ecs
