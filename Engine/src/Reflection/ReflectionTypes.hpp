#pragma once

#include <cstdint>
#include <cstddef>
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
    const char* category = "";
    const char* display_name = nullptr; // null = use field name
    const char* tooltip = "";
    float clamp_min = 0.0f;
    float clamp_max = 0.0f;
    bool has_clamp = false;
    float drag_speed = 0.1f;
    // For enum properties
    const char** enum_names = nullptr;
    int enum_count = 0;
};

// ---- Property descriptor ----

struct PropertyDescriptor
{
    const char* name;           // Field name (e.g. "speed")
    EPropertyType type;         // Data type
    uint32_t offset;            // offsetof() from component start
    uint32_t size;              // sizeof the field
    PropertyMeta meta;          // Editor metadata
};

// ---- Component descriptor ----

struct ComponentDescriptor
{
    const char* name;           // Type name (e.g. "PlayerComponent")
    const char* display_name;   // Editor display (e.g. "Player")
    const char* category;       // Grouping (e.g. "Gameplay")
    const char* source_id;      // "engine" or DLL name (for unload tracking)
    uint32_t type_id;           // entt::type_hash<T>::value()
    size_t size;                // sizeof(T)
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
