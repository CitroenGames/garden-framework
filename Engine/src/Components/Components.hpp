#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <memory>
#include <entt/entt.hpp>
#include "mesh.hpp"

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
};

struct TagComponent {
    std::string name;
};

struct MeshComponent {
    std::shared_ptr<mesh> m_mesh;
};

struct RigidBodyComponent {
    glm::vec3 velocity;
    glm::vec3 force;
    float mass = 1.0f;
    bool apply_gravity = true;
};

struct ColliderComponent {
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
    float speed = 1.5f;
    float jump_force = 3.0f;
    float mouse_sensitivity = 1.0f;
    bool grounded = false;
    glm::vec3 ground_normal;
    bool input_enabled = true;
};

struct FreecamComponent {
    float movement_speed = 5.0f;
    float fast_movement_speed = 15.0f;
    float mouse_sensitivity = 1.0f;
    bool input_enabled = true;
};

struct PlayerRepresentationComponent {
    entt::entity tracked_player = entt::null;
    glm::vec3 position_offset;
    bool visible_only_freecam = true;
};