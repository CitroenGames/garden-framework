#pragma once

#include "ReflectionTypes.hpp"
#include "Reflector.hpp"
#include <entt/entt.hpp>
#include <new>

// ============================================================================
// Helper template — builds a ComponentDescriptor with ECS bridge functions.
// Properties are populated by Reflector<T> via the component's reflect() method.
// ============================================================================

template<typename T>
ComponentDescriptor makeComponentDescriptor(
    const char* name,
    const char* source_id)
{
    ComponentDescriptor desc{};
    desc.name = name;
    desc.display_name = name;   // Overridden by Reflector::display()
    desc.category = "";         // Overridden by Reflector::category()
    desc.source_id = source_id;
    desc.type_id = entt::type_hash<T>::value();
    desc.size = sizeof(T);
    desc.removable = true;

    desc.add = [](entt::registry& r, entt::entity e) {
        if (!r.all_of<T>(e)) r.emplace<T>(e);
    };
    desc.remove = [](entt::registry& r, entt::entity e) {
        if (r.all_of<T>(e)) r.remove<T>(e);
    };
    desc.has = [](entt::registry& r, entt::entity e) -> bool {
        return r.all_of<T>(e);
    };
    desc.get = [](entt::registry& r, entt::entity e) -> void* {
        return r.all_of<T>(e) ? static_cast<void*>(&r.get<T>(e)) : nullptr;
    };
    desc.construct_default = [](void* dest) {
        new (dest) T{};
    };
    desc.destruct = [](void* dest) {
        static_cast<T*>(dest)->~T();
    };

    return desc;
}
