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

struct RigidBodyComponent {
    glm::vec3 velocity;
    glm::vec3 force;
    float mass = 1.0f;
    bool apply_gravity = true;

    static void reflect(Reflector<RigidBodyComponent>& r) {
        r.display("Rigid Body").category("Physics");
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

struct ColliderComponent {
    std::shared_ptr<mesh> m_mesh;
    // No reflect() — contains non-reflectable shared_ptr data, handled with custom UI

    bool is_mesh_valid() const
    {
        return m_mesh != nullptr && m_mesh->is_valid;
    }

    mesh* get_mesh() const
    {
        return (m_mesh != nullptr && m_mesh->is_valid) ? m_mesh.get() : nullptr;
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
