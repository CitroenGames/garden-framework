#pragma once

#include "ReflectionTypes.hpp"
#include "ReflectionRegistry.hpp"
#include <entt/entt.hpp>
#include <cstddef>
#include <new>

// ============================================================================
// GPROPERTY — Unreal-style property annotation.
// Place before fields in struct headers. Expands to nothing at compile time;
// serves as documentation and intent marker (like Unreal's UPROPERTY).
//
// Usage:
//   GPROPERTY(EditAnywhere, Category = "Movement")
//   float speed = 1.5f;
// ============================================================================
#define GPROPERTY(...)

// ============================================================================
// GCLASS — Unreal-style class annotation.
// Place inside component structs. Expands to nothing at compile time.
//
// Usage:
//   struct PlayerComponent
//   {
//       GCLASS(PlayerComponent)
//       ...
//   };
// ============================================================================
#define GCLASS(...)

// ============================================================================
// Helper template — builds ComponentDescriptor with ECS bridge functions.
// ============================================================================

template<typename T>
ComponentDescriptor makeComponentDescriptor(
    const char* name,
    const char* display_name,
    const char* category,
    const char* source_id,
    PropertyDescriptor* props,
    uint32_t prop_count)
{
    ComponentDescriptor desc{};
    desc.name = name;
    desc.display_name = display_name;
    desc.category = category;
    desc.source_id = source_id;
    desc.type_id = entt::type_hash<T>::value();
    desc.size = sizeof(T);
    desc.properties = props;
    desc.property_count = prop_count;

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

// ============================================================================
// Registration macros — used in .cpp files to define reflection data.
// Auto-computes field offsets via offsetof().
//
// Usage:
//   REFLECT_BEGIN(PlayerComponent, "Player", "Gameplay", "engine")
//       REFLECT_PROP(speed,      Float, EditAnywhere,    "Movement")
//       REFLECT_PROP(jump_force,  Float, EditAnywhere,    "Movement")
//       REFLECT_PROP(grounded,    Bool,  VisibleAnywhere, "State")
//   REFLECT_END(PlayerComponent, "Player", "Gameplay", "engine")
//
// This generates:  void registerReflection_PlayerComponent(ReflectionRegistry&);
// Call it to register the component.
// ============================================================================

#define REFLECT_BEGIN(Type, DisplayName, CategoryStr, SourceId)              \
    namespace _Refl_##Type {                                                 \
        using _Self = Type;                                                  \
        static PropertyDescriptor _props[] = {

// Basic property — widget auto-inferred from type
#define REFLECT_PROP(Name, PropType, Spec, Cat)                              \
    {                                                                        \
        #Name,                                                               \
        EPropertyType::PropType,                                             \
        (uint32_t)offsetof(_Self, Name),                                     \
        (uint32_t)sizeof(_Self::Name),                                       \
        PropertyMeta{ EPropertySpecifier::Spec, EPropertyWidget::Auto, Cat } \
    },

// Extended property — explicit widget, tooltip, drag speed, clamp range
#define REFLECT_PROP_EX(Name, PropType, Spec, Widget, Cat, Tip,              \
                        Speed, Min, Max)                                     \
    {                                                                        \
        #Name,                                                               \
        EPropertyType::PropType,                                             \
        (uint32_t)offsetof(_Self, Name),                                     \
        (uint32_t)sizeof(_Self::Name),                                       \
        PropertyMeta{                                                        \
            EPropertySpecifier::Spec,                                        \
            EPropertyWidget::Widget,                                         \
            Cat, nullptr, Tip,                                               \
            Min, Max, (Min != Max), Speed                                    \
        }                                                                    \
    },

#define REFLECT_END(Type, DisplayName, CategoryStr, SourceId)                \
        };                                                                   \
        static constexpr uint32_t _prop_count =                              \
            sizeof(_props) / sizeof(_props[0]);                              \
        static ComponentDescriptor _desc =                                   \
            makeComponentDescriptor<_Self>(                                   \
                #Type, DisplayName, CategoryStr, SourceId,                   \
                _props, _prop_count);                                        \
    }                                                                        \
    void registerReflection_##Type(ReflectionRegistry& registry)             \
    {                                                                        \
        registry.registerComponent(_Refl_##Type::_desc);                     \
    }

// Forward-declare a registration function (for use in headers)
#define DECLARE_REFLECT(Type)                                                \
    void registerReflection_##Type(ReflectionRegistry& registry);
