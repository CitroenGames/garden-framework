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
#include "Reflection/Reflect.hpp"

struct TransformComponent {
    GCLASS(TransformComponent)

    GPROPERTY(EditAnywhere, Category = "Transform")
    glm::vec3 position;

    GPROPERTY(EditAnywhere, Category = "Transform")
    glm::vec3 rotation;

    GPROPERTY(EditAnywhere, Category = "Transform")
    glm::vec3 scale;

    TransformComponent(float x=0, float y=0, float z=0) : position(x,y,z), rotation(0,0,0), scale(1,1,1) {}

    glm::mat4 getTransformMatrix() const {
        glm::mat4 scale_matrix = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 rotation_matrix = glm::eulerAngleYXZ(
            glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
        glm::mat4 translation_matrix = glm::translate(glm::mat4(1.0f), position);

        return translation_matrix * rotation_matrix * scale_matrix;
    }
};

struct TagComponent {
    GCLASS(TagComponent)

    GPROPERTY(EditAnywhere, Category = "Tag")
    std::string name;
};

struct MeshComponent {
    GCLASS(MeshComponent)
    std::shared_ptr<mesh> m_mesh;
};

struct RigidBodyComponent {
    GCLASS(RigidBodyComponent)

    GPROPERTY(VisibleAnywhere, Category = "Physics")
    glm::vec3 velocity;

    GPROPERTY(VisibleAnywhere, Category = "Physics")
    glm::vec3 force;

    GPROPERTY(EditAnywhere, Category = "Physics")
    float mass = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Physics")
    bool apply_gravity = true;
};

struct ColliderComponent {
    GCLASS(ColliderComponent)
    std::shared_ptr<mesh> m_mesh;

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
    GCLASS(PlayerComponent)

    GPROPERTY(EditAnywhere, Category = "Movement")
    float speed = 1.5f;

    GPROPERTY(EditAnywhere, Category = "Movement")
    float jump_force = 3.0f;

    GPROPERTY(EditAnywhere, Category = "Input")
    float mouse_sensitivity = 1.0f;

    GPROPERTY(VisibleAnywhere, Category = "State")
    bool grounded = false;

    glm::vec3 ground_normal = glm::vec3(0, 1, 0);

    GPROPERTY(EditAnywhere, Category = "Input")
    bool input_enabled = true;

    GPROPERTY(EditAnywhere, Category = "Collision")
    float capsule_half_height = 0.9f;

    GPROPERTY(EditAnywhere, Category = "Collision")
    float capsule_radius = 0.3f;
};

struct FreecamComponent {
    GCLASS(FreecamComponent)

    GPROPERTY(EditAnywhere, Category = "Movement")
    float movement_speed = 5.0f;

    GPROPERTY(EditAnywhere, Category = "Movement")
    float fast_movement_speed = 15.0f;

    GPROPERTY(EditAnywhere, Category = "Input")
    float mouse_sensitivity = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Input")
    bool input_enabled = true;
};

struct PlayerRepresentationComponent {
    GCLASS(PlayerRepresentationComponent)

    GPROPERTY(EditAnywhere, Category = "Tracking")
    entt::entity tracked_player = entt::null;

    GPROPERTY(EditAnywhere, Category = "Tracking")
    glm::vec3 position_offset;

    GPROPERTY(EditAnywhere, Category = "Tracking")
    bool visible_only_freecam = true;
};

struct PointLightComponent {
    GCLASS(PointLightComponent)

    GPROPERTY(EditAnywhere, Category = "Light")
    glm::vec3 color{1.0f, 1.0f, 1.0f};

    GPROPERTY(EditAnywhere, Category = "Light")
    float intensity = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Light")
    float range = 10.0f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float constant_attenuation = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float linear_attenuation = 0.09f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float quadratic_attenuation = 0.032f;
};

struct SpotLightComponent {
    GCLASS(SpotLightComponent)

    GPROPERTY(EditAnywhere, Category = "Light")
    glm::vec3 color{1.0f, 1.0f, 1.0f};

    GPROPERTY(EditAnywhere, Category = "Light")
    float intensity = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Light")
    float range = 15.0f;

    GPROPERTY(EditAnywhere, Category = "Light")
    float inner_cone_angle = 12.5f;

    GPROPERTY(EditAnywhere, Category = "Light")
    float outer_cone_angle = 17.5f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float constant_attenuation = 1.0f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float linear_attenuation = 0.09f;

    GPROPERTY(EditAnywhere, Category = "Attenuation")
    float quadratic_attenuation = 0.032f;
};