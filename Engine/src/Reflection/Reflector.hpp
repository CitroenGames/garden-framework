#pragma once

#include "ReflectionTypes.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <initializer_list>
#include <string>
#include <type_traits>

// ============================================================================
// Auto type deduction - maps C++ field types to reflected property kinds.
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

template<typename M>
struct DeducePropertyType<M, std::enable_if_t<std::is_enum_v<M>>>
{
    static constexpr auto value = EPropertyType::Enum;
};

template<typename>
struct ReflectionMemberPointerTraits;

template<typename OwnerT, typename FieldT>
struct ReflectionMemberPointerTraits<FieldT OwnerT::*>
{
    using Owner = OwnerT;
    using Field = FieldT;
};

// ============================================================================
// PropertyBuilder - fluent API for configuring a single reflected field.
// ============================================================================

class PropertyBuilder
{
    std::vector<PropertyDescriptor>& m_props;
    size_t m_index = 0;

public:
    PropertyBuilder(std::vector<PropertyDescriptor>& props, size_t idx)
        : m_props(props), m_index(idx) {}

    PropertyBuilder& tooltip(std::string t)
    { m_props[m_index].meta.tooltip = std::move(t); return *this; }

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

    PropertyBuilder& category(std::string c)
    { m_props[m_index].meta.category = std::move(c); return *this; }

    PropertyBuilder& widget(EPropertyWidget w)
    { m_props[m_index].meta.widget = w; return *this; }

    PropertyBuilder& display(std::string name)
    { m_props[m_index].meta.display_name = std::move(name); return *this; }

    PropertyBuilder& enumValues(std::initializer_list<const char*> names)
    {
        auto& prop = m_props[m_index];
        prop.meta.enum_names.clear();
        prop.meta.enum_names.reserve(names.size());
        for (const char* name : names)
            prop.meta.enum_names.emplace_back(name ? name : "");
        prop.type = EPropertyType::Enum;
        return *this;
    }

    PropertyBuilder& enumValues(const char* const* names, int count)
    {
        auto& prop = m_props[m_index];
        prop.meta.enum_names.clear();
        if (names && count > 0)
        {
            prop.meta.enum_names.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
                prop.meta.enum_names.emplace_back(names[i] ? names[i] : "");
        }
        prop.type = EPropertyType::Enum;
        return *this;
    }
};

// ============================================================================
// Reflector<T> - explicit, typed builder API for component metadata.
//
// Usage:
//   r.field<&MyComponent::speed>("speed").tooltip("Movement speed");
// ============================================================================

template<typename T>
class Reflector
{
    ComponentDescriptor& m_desc;

public:
    explicit Reflector(ComponentDescriptor& desc) : m_desc(desc) {}

    Reflector& display(std::string d)
    { m_desc.display_name = std::move(d); return *this; }

    Reflector& category(std::string c)
    { m_desc.category = std::move(c); return *this; }

    Reflector& removable(bool r)
    { m_desc.removable = r; return *this; }

    template<auto Member>
    PropertyBuilder field(std::string name)
    {
        using MemberInfo = ReflectionMemberPointerTraits<decltype(Member)>;
        using Owner = typename MemberInfo::Owner;
        using Field = typename MemberInfo::Field;

        static_assert(std::is_same_v<Owner, T>, "Reflected field must belong to this component type");

        PropertyDescriptor prop{};
        prop.name = std::move(name);
        prop.type = DeducePropertyType<Field>::value;
        prop.size = static_cast<uint32_t>(sizeof(Field));
        prop.mutable_data = [](void* component) -> void* {
            return static_cast<void*>(&(static_cast<T*>(component)->*Member));
        };
        prop.const_data = [](const void* component) -> const void* {
            return static_cast<const void*>(&(static_cast<const T*>(component)->*Member));
        };

        m_desc.properties.push_back(std::move(prop));
        return PropertyBuilder(m_desc.properties, m_desc.properties.size() - 1);
    }

    template<typename M>
    PropertyBuilder property(const char* name, M T::* member) = delete;
};
