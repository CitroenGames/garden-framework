#include "EngineReflection.hpp"
#include "Reflect.hpp"
#include "Components/Components.hpp"

// ============================================================================
// TransformComponent
// ============================================================================
REFLECT_BEGIN(TransformComponent, "Transform", "Core", "engine")
    REFLECT_PROP_EX(position, Vec3, EditAnywhere, DragFloat3, "Transform",
                    "World position", 0.01f, 0.0f, 0.0f)
    REFLECT_PROP_EX(rotation, Vec3, EditAnywhere, DragFloat3, "Transform",
                    "Euler rotation (degrees)", 0.5f, 0.0f, 0.0f)
    REFLECT_PROP_EX(scale,    Vec3, EditAnywhere, DragFloat3, "Transform",
                    "Scale", 0.01f, 0.001f, 1000.0f)
REFLECT_END(TransformComponent, "Transform", "Core", "engine")

// ============================================================================
// TagComponent
// ============================================================================
REFLECT_BEGIN(TagComponent, "Tag", "Core", "engine")
    REFLECT_PROP(name, String, EditAnywhere, "Tag")
REFLECT_END(TagComponent, "Tag", "Core", "engine")

// ============================================================================
// RigidBodyComponent
// ============================================================================
REFLECT_BEGIN(RigidBodyComponent, "Rigid Body", "Physics", "engine")
    REFLECT_PROP(velocity,      Vec3,  VisibleAnywhere, "Physics")
    REFLECT_PROP(force,         Vec3,  VisibleAnywhere, "Physics")
    REFLECT_PROP_EX(mass,       Float, EditAnywhere, DragFloat, "Physics",
                    "Body mass", 0.1f, 0.001f, 100000.0f)
    REFLECT_PROP(apply_gravity, Bool,  EditAnywhere, "Physics")
REFLECT_END(RigidBodyComponent, "Rigid Body", "Physics", "engine")

// ============================================================================
// PlayerComponent
// ============================================================================
REFLECT_BEGIN(PlayerComponent, "Player", "Gameplay", "engine")
    REFLECT_PROP_EX(speed,             Float, EditAnywhere, DragFloat, "Movement",
                    "Movement speed", 0.1f, 0.0f, 100.0f)
    REFLECT_PROP_EX(jump_force,        Float, EditAnywhere, DragFloat, "Movement",
                    "Jump force", 0.1f, 0.0f, 100.0f)
    REFLECT_PROP_EX(mouse_sensitivity, Float, EditAnywhere, DragFloat, "Input",
                    "Mouse sensitivity", 0.01f, 0.01f, 10.0f)
    REFLECT_PROP(grounded,             Bool,  VisibleAnywhere, "State")
    REFLECT_PROP(input_enabled,        Bool,  EditAnywhere,    "Input")
    REFLECT_PROP_EX(capsule_half_height, Float, EditAnywhere, DragFloat, "Collision",
                    "Capsule half height", 0.01f, 0.01f, 10.0f)
    REFLECT_PROP_EX(capsule_radius,    Float, EditAnywhere, DragFloat, "Collision",
                    "Capsule radius", 0.01f, 0.01f, 5.0f)
REFLECT_END(PlayerComponent, "Player", "Gameplay", "engine")

// ============================================================================
// FreecamComponent
// ============================================================================
REFLECT_BEGIN(FreecamComponent, "Freecam", "Gameplay", "engine")
    REFLECT_PROP_EX(movement_speed,      Float, EditAnywhere, DragFloat, "Movement",
                    "Normal speed", 0.1f, 0.0f, 100.0f)
    REFLECT_PROP_EX(fast_movement_speed, Float, EditAnywhere, DragFloat, "Movement",
                    "Fast speed (shift)", 0.1f, 0.0f, 200.0f)
    REFLECT_PROP_EX(mouse_sensitivity,   Float, EditAnywhere, DragFloat, "Input",
                    "Mouse sensitivity", 0.01f, 0.01f, 10.0f)
    REFLECT_PROP(input_enabled,          Bool,  EditAnywhere, "Input")
REFLECT_END(FreecamComponent, "Freecam", "Gameplay", "engine")

// ============================================================================
// PointLightComponent
// ============================================================================
REFLECT_BEGIN(PointLightComponent, "Point Light", "Lighting", "engine")
    REFLECT_PROP_EX(color,                   Vec3,  EditAnywhere, ColorEdit3, "Light",
                    "Light color", 0.0f, 0.0f, 0.0f)
    REFLECT_PROP_EX(intensity,               Float, EditAnywhere, DragFloat, "Light",
                    "Light intensity", 0.1f, 0.0f, 100.0f)
    REFLECT_PROP_EX(range,                   Float, EditAnywhere, DragFloat, "Light",
                    "Light range", 0.1f, 0.1f, 1000.0f)
    REFLECT_PROP_EX(constant_attenuation,    Float, EditAnywhere, DragFloat, "Attenuation",
                    "Constant", 0.01f, 0.0f, 10.0f)
    REFLECT_PROP_EX(linear_attenuation,      Float, EditAnywhere, DragFloat, "Attenuation",
                    "Linear", 0.001f, 0.0f, 2.0f)
    REFLECT_PROP_EX(quadratic_attenuation,   Float, EditAnywhere, DragFloat, "Attenuation",
                    "Quadratic", 0.001f, 0.0f, 2.0f)
REFLECT_END(PointLightComponent, "Point Light", "Lighting", "engine")

// ============================================================================
// SpotLightComponent
// ============================================================================
REFLECT_BEGIN(SpotLightComponent, "Spot Light", "Lighting", "engine")
    REFLECT_PROP_EX(color,                   Vec3,  EditAnywhere, ColorEdit3, "Light",
                    "Light color", 0.0f, 0.0f, 0.0f)
    REFLECT_PROP_EX(intensity,               Float, EditAnywhere, DragFloat, "Light",
                    "Light intensity", 0.1f, 0.0f, 100.0f)
    REFLECT_PROP_EX(range,                   Float, EditAnywhere, DragFloat, "Light",
                    "Light range", 0.1f, 0.1f, 1000.0f)
    REFLECT_PROP_EX(inner_cone_angle,        Float, EditAnywhere, DragFloat, "Light",
                    "Inner cone angle (degrees)", 0.5f, 0.0f, 90.0f)
    REFLECT_PROP_EX(outer_cone_angle,        Float, EditAnywhere, DragFloat, "Light",
                    "Outer cone angle (degrees)", 0.5f, 0.0f, 90.0f)
    REFLECT_PROP_EX(constant_attenuation,    Float, EditAnywhere, DragFloat, "Attenuation",
                    "Constant", 0.01f, 0.0f, 10.0f)
    REFLECT_PROP_EX(linear_attenuation,      Float, EditAnywhere, DragFloat, "Attenuation",
                    "Linear", 0.001f, 0.0f, 2.0f)
    REFLECT_PROP_EX(quadratic_attenuation,   Float, EditAnywhere, DragFloat, "Attenuation",
                    "Quadratic", 0.001f, 0.0f, 2.0f)
REFLECT_END(SpotLightComponent, "Spot Light", "Lighting", "engine")

// ============================================================================
// Aggregate registration
// ============================================================================
void registerEngineReflection(ReflectionRegistry& registry)
{
    registerReflection_TransformComponent(registry);
    registerReflection_TagComponent(registry);
    registerReflection_RigidBodyComponent(registry);
    registerReflection_PlayerComponent(registry);
    registerReflection_FreecamComponent(registry);
    registerReflection_PointLightComponent(registry);
    registerReflection_SpotLightComponent(registry);
}
