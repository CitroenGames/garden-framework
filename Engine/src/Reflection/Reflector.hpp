#pragma once

#include "ReflectionTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <entt/entt.hpp>

// ============================================================================
// Auto type deduction — maps C++ types to EPropertyType
// ============================================================================

template<typename M, typename Enable = void>
struct DeducePropertyType;

template<> struct DeducePropertyType<float>        { static constexpr auto value = EPropertyType::Float; };
template<> struct DeducePropertyType<int>          { static constexpr auto value = EPropertyType::Int; };
template<> struct DeducePropertyType<bool>         { static constexpr auto value = EPropertyType::Bool; };
template<> struct DeducePropertyType<std::string>  { static constexpr auto value = EPropertyType::String; };
template<> struct DeducePropertyType<glm::vec2>    { static constexpr auto value = EPropertyType::Vec2; };
template<> struct DeducePropertyType<glm::vec3>    { static constexpr auto value = EPropertyType::Vec3; };
template<> struct DeducePropertyType<glm::vec4>    { static constexpr auto value = EPropertyType::Vec4; };
template<> struct DeducePropertyType<glm::quat>    { static constexpr auto value = EPropertyType::Quat; };
template<> struct DeducePropertyType<glm::mat4>    { static constexpr auto value = EPropertyType::Mat4; };
template<> struct DeducePropertyType<entt::entity> { static constexpr auto value = EPropertyType::Entity; };

// Fallback for enum types
template<typename M>
struct DeducePropertyType<M, std::enable_if_t<std::is_enum_v<M>>>
{
    static constexpr auto value = EPropertyType::Enum;
};

// ============================================================================
// Member pointer to byte offset conversion
// ============================================================================

template<typename T, typename M>
uint32_t memberOffset(M T::* member)
{
    return static_cast<uint32_t>(
        reinterpret_cast<std::uintptr_t>(
            &(static_cast<T*>(nullptr)->*member)
        )
    );
}

// ============================================================================
// PropertyBuilder — fluent API for configuring a single property
// Uses index-based storage to avoid dangling references on vector reallocation.
// ============================================================================

class PropertyBuilder
{
    std::vector<PropertyDescriptor>& m_props;
    size_t m_index;

public:
    PropertyBuilder(std::vector<PropertyDescriptor>& props, size_t idx)
        : m_props(props), m_index(idx) {}

    PropertyBuilder& tooltip(const char* t)
    { m_props[m_index].meta.tooltip = t; return *this; }

    PropertyBuilder& drag(float speed)
    { m_props[m_index].meta.drag_speed = speed; return *this; }

    PropertyBuilder& range(float min_val, float max_val)
    {
        m_props[m_index].meta.clamp_min = min_val;
        m_props[m_index].meta.clamp_max = max_val;
        m_props[m_index].meta.has_clamp = (min_val != max_val);
        return *this;
    }

    PropertyBuilder& visible()
    {
        m_props[m_index].meta.specifier = EPropertySpecifier::VisibleAnywhere;
        return *this;
    }

    PropertyBuilder& category(const char* c)
    { m_props[m_index].meta.category = c; return *this; }

    PropertyBuilder& widget(EPropertyWidget w)
    { m_props[m_index].meta.widget = w; return *this; }

    PropertyBuilder& display(const char* name)
    { m_props[m_index].meta.display_name = name; return *this; }

    PropertyBuilder& enumValues(const char** names, int count)
    {
        m_props[m_index].meta.enum_names = names;
        m_props[m_index].meta.enum_count = count;
        m_props[m_index].type = EPropertyType::Enum;
        return *this;
    }
};

// ============================================================================
// Reflector<T> — fluent API for defining component reflection metadata
//
// Usage inside a component struct:
//   static void reflect(Reflector<MyComponent>& r) {
//       r.display("My Component").category("Gameplay");
//       r.property("speed", &MyComponent::speed)
//           .tooltip("Movement speed").drag(0.1f).range(0.0f, 100.0f);
//   }
// ============================================================================

template<typename T>
class Reflector
{
    ComponentDescriptor& m_desc;

public:
    explicit Reflector(ComponentDescriptor& desc) : m_desc(desc) {}

    Reflector& display(const char* d)
    { m_desc.display_name = d; return *this; }

    Reflector& category(const char* c)
    { m_desc.category = c; return *this; }

    Reflector& removable(bool r)
    { m_desc.removable = r; return *this; }

    template<typename M>
    PropertyBuilder property(const char* name, M T::* member)
    {
        PropertyDescriptor prop{};
        prop.name = name;
        prop.type = DeducePropertyType<M>::value;
        prop.offset = memberOffset<T, M>(member);
        prop.size = static_cast<uint32_t>(sizeof(M));

        m_desc.properties.push_back(prop);
        return PropertyBuilder(m_desc.properties, m_desc.properties.size() - 1);
    }
};
