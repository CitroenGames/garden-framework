#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <entt/entt.hpp>

// ---- Property specifiers (Unreal-style) ----

enum class EPropertySpecifier : uint8_t
{
    EditAnywhere,       // Editable in inspector on any instance
    VisibleAnywhere,    // Visible but read-only
    EditDefaultsOnly,   // Only editable on archetype/default
};

// ---- Property types ----

enum class EPropertyType : uint8_t
{
    Float,
    Int,
    Bool,
    String,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    Entity,
    AssetPath,
    Enum,
};

// ---- Property widget override ----

enum class EPropertyWidget : uint8_t
{
    Auto,           // Infer from type
    DragFloat,
    SliderFloat,
    DragInt,
    SliderInt,
    Checkbox,
    InputText,
    ColorEdit3,     // Vec3 as RGB
    ColorEdit4,     // Vec4 as RGBA
    DragFloat2,
    DragFloat3,
    DragFloat4,
    EntityRef,
    AssetPath,
    Enum,
    ReadOnly,
};

// ---- Property metadata ----

struct PropertyMeta
{
    EPropertySpecifier specifier = EPropertySpecifier::EditAnywhere;
    EPropertyWidget widget = EPropertyWidget::Auto;
    std::string category;
    std::string display_name; // empty = use field name
    std::string tooltip;
    float clamp_min = 0.0f;
    float clamp_max = 0.0f;
    bool has_clamp = false;
    float drag_speed = 0.1f;
    std::vector<std::string> enum_names;
};

// ---- Property descriptor ----

struct PropertyDescriptor
{
    std::string name;          // Stable serialized field name (e.g. "speed")
    EPropertyType type;        // Data type
    uint32_t size = 0;         // sizeof the field
    PropertyMeta meta;         // Editor metadata

    void* (*mutable_data)(void* component) = nullptr;
    const void* (*const_data)(const void* component) = nullptr;
};

// ---- Component descriptor ----

struct ComponentDescriptor
{
    std::string name;          // Stable serialized type name (e.g. "PlayerComponent")
    std::string display_name;  // Editor display (e.g. "Player")
    std::string category;      // Grouping (e.g. "Gameplay")
    std::string source_id;     // "engine" or DLL name (for unload tracking)
    uint32_t type_id = 0;      // entt::type_hash<T>::value()
    size_t size = 0;           // sizeof(T)
    bool removable = true;      // Can be removed in editor inspector

    std::vector<PropertyDescriptor> properties;

    // ECS bridge — function pointers compiled where T is known
    void  (*add)(entt::registry&, entt::entity)      = nullptr;
    void  (*remove)(entt::registry&, entt::entity)   = nullptr;
    bool  (*has)(entt::registry&, entt::entity)       = nullptr;
    void* (*get)(entt::registry&, entt::entity)      = nullptr;  // nullptr if absent
    void  (*construct_default)(void* dest)            = nullptr;  // placement-new
    void  (*destruct)(void* dest)                     = nullptr;  // destructor call
};
