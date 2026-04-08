#pragma once
#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <memory>
#include <entt/entt.hpp>
#include "mesh.hpp"
#include "Reflection/Reflector.hpp"

struct TransformComponent {
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;

    TransformComponent(float x=0, float y=0, float z=0) : position(x,y,z), rotation(0,0,0), scale(1,1,1) {}

    glm::mat4 getTransformMatrix() const {
        glm::mat4 scale_matrix = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 rotation_matrix = glm::eulerAngleYXZ(
            glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
        glm::mat4 translation_matrix = glm::translate(glm::mat4(1.0f), position);

        return translation_matrix * rotation_matrix * scale_matrix;
    }

    static void reflect(Reflector<TransformComponent>& r) {
        r.display("Transform").category("Core").removable(false);
        r.property("position", &TransformComponent::position)
            .tooltip("World position").drag(0.01f).category("Transform");
        r.property("rotation", &TransformComponent::rotation)
            .tooltip("Euler rotation (degrees)").drag(0.5f).category("Transform");
        r.property("scale", &TransformComponent::scale)
            .tooltip("Scale").drag(0.01f).range(0.001f, 1000.0f).category("Transform");
    }
};

struct TagComponent {
    std::string name;

    static void reflect(Reflector<TagComponent>& r) {
        r.display("Tag").category("Core").removable(false);
        r.property("name", &TagComponent::name).category("Tag");
    }
};

struct MeshComponent {
    std::shared_ptr<mesh> m_mesh;
    // No reflect() — contains non-reflectable shared_ptr data, handled with custom UI
};

enum class BodyMotionType : int
{
    Dynamic = 0,
    Kinematic,
    Static,
    COUNT
};

static const char* body_motion_type_names[] = { "Dynamic", "Kinematic", "Static" };

inline BodyMotionType stringToBodyMotionType(const std::string& s)
{
    if (s == "Kinematic") return BodyMotionType::Kinematic;
    if (s == "Static")    return BodyMotionType::Static;
    return BodyMotionType::Dynamic;
}

inline std::string bodyMotionTypeToString(BodyMotionType t)
{
    switch (t)
    {
    case BodyMotionType::Kinematic: return "Kinematic";
    case BodyMotionType::Static:    return "Static";
    default:                        return "Dynamic";
    }
}

struct RigidBodyComponent {
    glm::vec3 velocity;
    glm::vec3 force;
    float mass = 1.0f;
    bool apply_gravity = true;
    BodyMotionType motion_type = BodyMotionType::Dynamic;

    static void reflect(Reflector<RigidBodyComponent>& r) {
        r.display("Rigid Body").category("Physics");
        r.property("motion_type", &RigidBodyComponent::motion_type)
            .tooltip("Body motion type")
            .enumValues(body_motion_type_names, (int)BodyMotionType::COUNT)
            .category("Physics");
        r.property("velocity", &RigidBodyComponent::velocity)
            .visible().category("Physics");
        r.property("force", &RigidBodyComponent::force)
            .visible().category("Physics");
        r.property("mass", &RigidBodyComponent::mass)
            .tooltip("Body mass").drag(0.1f).range(0.001f, 100000.0f).category("Physics");
        r.property("apply_gravity", &RigidBodyComponent::apply_gravity)
            .category("Physics");
    }
};

enum class ColliderShapeType : int
{
    Mesh = 0,       // Triangle mesh (static only)
    Box,            // Box with half extents
    Sphere,         // Sphere with radius
    Capsule,        // Capsule with half height and radius
    Cylinder,       // Cylinder with half height and radius
    ConvexHull,     // Convex hull from mesh vertices
    COUNT
};

static const char* collider_shape_type_names[] = {
    "Mesh", "Box", "Sphere", "Capsule", "Cylinder", "ConvexHull"
};

inline ColliderShapeType stringToColliderShapeType(const std::string& s)
{
    if (s == "Box")       return ColliderShapeType::Box;
    if (s == "Sphere")    return ColliderShapeType::Sphere;
    if (s == "Capsule")   return ColliderShapeType::Capsule;
    if (s == "Cylinder")  return ColliderShapeType::Cylinder;
    if (s == "ConvexHull")return ColliderShapeType::ConvexHull;
    return ColliderShapeType::Mesh;
}

inline std::string colliderShapeTypeToString(ColliderShapeType t)
{
    switch (t)
    {
    case ColliderShapeType::Box:       return "Box";
    case ColliderShapeType::Sphere:    return "Sphere";
    case ColliderShapeType::Capsule:   return "Capsule";
    case ColliderShapeType::Cylinder:  return "Cylinder";
    case ColliderShapeType::ConvexHull:return "ConvexHull";
    default:                           return "Mesh";
    }
}

struct ColliderComponent {
    std::shared_ptr<mesh> m_mesh;  // Used for Mesh and ConvexHull shape types

    ColliderShapeType shape_type = ColliderShapeType::Mesh;

    // Primitive shape parameters
    glm::vec3 box_half_extents = glm::vec3(0.5f);
    float sphere_radius = 0.5f;
    float capsule_half_height = 0.5f;
    float capsule_radius = 0.3f;
    float cylinder_half_height = 0.5f;
    float cylinder_radius = 0.5f;

    // Physics material
    float friction = 0.2f;
    float restitution = 0.0f;

    bool is_mesh_valid() const
    {
        return m_mesh != nullptr && m_mesh->is_valid;
    }

    mesh* get_mesh() const
    {
        return (m_mesh != nullptr && m_mesh->is_valid) ? m_mesh.get() : nullptr;
    }

    static void reflect(Reflector<ColliderComponent>& r) {
        r.display("Collider").category("Physics");
        r.property("shape_type", &ColliderComponent::shape_type)
            .tooltip("Collision shape type")
            .enumValues(collider_shape_type_names, (int)ColliderShapeType::COUNT)
            .category("Shape");
        r.property("box_half_extents", &ColliderComponent::box_half_extents)
            .tooltip("Box half extents").drag(0.01f).category("Shape");
        r.property("sphere_radius", &ColliderComponent::sphere_radius)
            .tooltip("Sphere radius").drag(0.01f).range(0.001f, 1000.0f).category("Shape");
        r.property("capsule_half_height", &ColliderComponent::capsule_half_height)
            .tooltip("Capsule half height").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.property("capsule_radius", &ColliderComponent::capsule_radius)
            .tooltip("Capsule radius").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.property("cylinder_half_height", &ColliderComponent::cylinder_half_height)
            .tooltip("Cylinder half height").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.property("cylinder_radius", &ColliderComponent::cylinder_radius)
            .tooltip("Cylinder radius").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.property("friction", &ColliderComponent::friction)
            .tooltip("Surface friction").drag(0.01f).range(0.0f, 10.0f).category("Material");
        r.property("restitution", &ColliderComponent::restitution)
            .tooltip("Bounciness").drag(0.01f).range(0.0f, 1.0f).category("Material");
    }
};

struct PlayerComponent {
    float speed = 1.5f;
    float jump_force = 3.0f;
    float mouse_sensitivity = 1.0f;
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0, 1, 0);
    bool input_enabled = true;
    float capsule_half_height = 0.9f;
    float capsule_radius = 0.3f;

    static void reflect(Reflector<PlayerComponent>& r) {
        r.display("Player").category("Gameplay");
        r.property("speed", &PlayerComponent::speed)
            .tooltip("Movement speed").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.property("jump_force", &PlayerComponent::jump_force)
            .tooltip("Jump force").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.property("mouse_sensitivity", &PlayerComponent::mouse_sensitivity)
            .tooltip("Mouse sensitivity").drag(0.01f).range(0.01f, 10.0f).category("Input");
        r.property("grounded", &PlayerComponent::grounded)
            .visible().category("State");
        r.property("input_enabled", &PlayerComponent::input_enabled)
            .category("Input");
        r.property("capsule_half_height", &PlayerComponent::capsule_half_height)
            .tooltip("Capsule half height").drag(0.01f).range(0.01f, 10.0f).category("Collision");
        r.property("capsule_radius", &PlayerComponent::capsule_radius)
            .tooltip("Capsule radius").drag(0.01f).range(0.01f, 5.0f).category("Collision");
    }
};

struct FreecamComponent {
    float movement_speed = 5.0f;
    float fast_movement_speed = 15.0f;
    float mouse_sensitivity = 1.0f;
    bool input_enabled = true;

    static void reflect(Reflector<FreecamComponent>& r) {
        r.display("Freecam").category("Gameplay");
        r.property("movement_speed", &FreecamComponent::movement_speed)
            .tooltip("Normal speed").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.property("fast_movement_speed", &FreecamComponent::fast_movement_speed)
            .tooltip("Fast speed (shift)").drag(0.1f).range(0.0f, 200.0f).category("Movement");
        r.property("mouse_sensitivity", &FreecamComponent::mouse_sensitivity)
            .tooltip("Mouse sensitivity").drag(0.01f).range(0.01f, 10.0f).category("Input");
        r.property("input_enabled", &FreecamComponent::input_enabled)
            .category("Input");
    }
};

struct PlayerRepresentationComponent {
    entt::entity tracked_player = entt::null;
    glm::vec3 position_offset;
    bool visible_only_freecam = true;

    static void reflect(Reflector<PlayerRepresentationComponent>& r) {
        r.display("Player Representation").category("Gameplay");
        r.property("tracked_player", &PlayerRepresentationComponent::tracked_player)
            .tooltip("Entity to track").category("Tracking");
        r.property("position_offset", &PlayerRepresentationComponent::position_offset)
            .tooltip("Offset from tracked entity").drag(0.01f).category("Tracking");
        r.property("visible_only_freecam", &PlayerRepresentationComponent::visible_only_freecam)
            .tooltip("Only visible in freecam mode").category("Tracking");
    }
};

struct PointLightComponent {
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float constant_attenuation = 1.0f;
    float linear_attenuation = 0.09f;
    float quadratic_attenuation = 0.032f;

    static void reflect(Reflector<PointLightComponent>& r) {
        r.display("Point Light").category("Lighting");
        r.property("color", &PointLightComponent::color)
            .tooltip("Light color").widget(EPropertyWidget::ColorEdit3).category("Light");
        r.property("intensity", &PointLightComponent::intensity)
            .tooltip("Light intensity").drag(0.1f).range(0.0f, 100.0f).category("Light");
        r.property("range", &PointLightComponent::range)
            .tooltip("Light range").drag(0.1f).range(0.1f, 1000.0f).category("Light");
        r.property("constant_attenuation", &PointLightComponent::constant_attenuation)
            .tooltip("Constant").drag(0.01f).range(0.0f, 10.0f).category("Attenuation");
        r.property("linear_attenuation", &PointLightComponent::linear_attenuation)
            .tooltip("Linear").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
        r.property("quadratic_attenuation", &PointLightComponent::quadratic_attenuation)
            .tooltip("Quadratic").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
    }
};

struct SpotLightComponent {
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 15.0f;
    float inner_cone_angle = 12.5f;
    float outer_cone_angle = 17.5f;
    float constant_attenuation = 1.0f;
    float linear_attenuation = 0.09f;
    float quadratic_attenuation = 0.032f;

    static void reflect(Reflector<SpotLightComponent>& r) {
        r.display("Spot Light").category("Lighting");
        r.property("color", &SpotLightComponent::color)
            .tooltip("Light color").widget(EPropertyWidget::ColorEdit3).category("Light");
        r.property("intensity", &SpotLightComponent::intensity)
            .tooltip("Light intensity").drag(0.1f).range(0.0f, 100.0f).category("Light");
        r.property("range", &SpotLightComponent::range)
            .tooltip("Light range").drag(0.1f).range(0.1f, 1000.0f).category("Light");
        r.property("inner_cone_angle", &SpotLightComponent::inner_cone_angle)
            .tooltip("Inner cone angle (degrees)").drag(0.5f).range(0.0f, 90.0f).category("Light");
        r.property("outer_cone_angle", &SpotLightComponent::outer_cone_angle)
            .tooltip("Outer cone angle (degrees)").drag(0.5f).range(0.0f, 90.0f).category("Light");
        r.property("constant_attenuation", &SpotLightComponent::constant_attenuation)
            .tooltip("Constant").drag(0.01f).range(0.0f, 10.0f).category("Attenuation");
        r.property("linear_attenuation", &SpotLightComponent::linear_attenuation)
            .tooltip("Linear").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
        r.property("quadratic_attenuation", &SpotLightComponent::quadratic_attenuation)
            .tooltip("Quadratic").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
    }
};

// --- Constraint System ---

enum class ConstraintType : int
{
    Fixed = 0,
    Hinge,
    Point,
    Distance,
    COUNT
};

static const char* constraint_type_names[] = { "Fixed", "Hinge", "Point", "Distance" };

inline ConstraintType stringToConstraintType(const std::string& s)
{
    if (s == "Hinge")    return ConstraintType::Hinge;
    if (s == "Point")    return ConstraintType::Point;
    if (s == "Distance") return ConstraintType::Distance;
    return ConstraintType::Fixed;
}

inline std::string constraintTypeToString(ConstraintType t)
{
    switch (t)
    {
    case ConstraintType::Hinge:    return "Hinge";
    case ConstraintType::Point:    return "Point";
    case ConstraintType::Distance: return "Distance";
    default:                       return "Fixed";
    }
}

struct ConstraintComponent {
    ConstraintType type = ConstraintType::Fixed;
    std::string target_entity_name;
    entt::entity target_entity = entt::null;

    // Attachment points (local space of each body)
    glm::vec3 anchor_1 = glm::vec3(0.0f);
    glm::vec3 anchor_2 = glm::vec3(0.0f);

    // Hinge-specific
    glm::vec3 hinge_axis = glm::vec3(0.0f, 1.0f, 0.0f);
    float hinge_min_limit = -180.0f;
    float hinge_max_limit = 180.0f;

    // Distance-specific
    float min_distance = -1.0f;  // -1 = auto-detect from initial positions
    float max_distance = -1.0f;

    static void reflect(Reflector<ConstraintComponent>& r) {
        r.display("Constraint").category("Physics");
        r.property("type", &ConstraintComponent::type)
            .tooltip("Constraint type")
            .enumValues(constraint_type_names, (int)ConstraintType::COUNT)
            .category("Constraint");
        r.property("target_entity_name", &ConstraintComponent::target_entity_name)
            .tooltip("Name of target entity").category("Constraint");
        r.property("anchor_1", &ConstraintComponent::anchor_1)
            .tooltip("Local anchor on this body").drag(0.01f).category("Constraint");
        r.property("anchor_2", &ConstraintComponent::anchor_2)
            .tooltip("Local anchor on target body").drag(0.01f).category("Constraint");
        r.property("hinge_axis", &ConstraintComponent::hinge_axis)
            .tooltip("Hinge rotation axis").drag(0.01f).category("Hinge");
        r.property("hinge_min_limit", &ConstraintComponent::hinge_min_limit)
            .tooltip("Min angle (degrees)").drag(1.0f).range(-180.0f, 0.0f).category("Hinge");
        r.property("hinge_max_limit", &ConstraintComponent::hinge_max_limit)
            .tooltip("Max angle (degrees)").drag(1.0f).range(0.0f, 180.0f).category("Hinge");
        r.property("min_distance", &ConstraintComponent::min_distance)
            .tooltip("Min distance (-1 = auto)").drag(0.01f).category("Distance");
        r.property("max_distance", &ConstraintComponent::max_distance)
            .tooltip("Max distance (-1 = auto)").drag(0.01f).category("Distance");
    }
};
