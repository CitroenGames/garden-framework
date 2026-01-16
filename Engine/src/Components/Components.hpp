#pragma once
#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/quaternion.h"
#include <string>
#include <memory>
#include <entt/entt.hpp>
#include "mesh.hpp"

using namespace irr;
using namespace core;

struct TransformComponent {
    vector3f position;
    vector3f rotation;
    vector3f scale;
    
    TransformComponent(float x=0, float y=0, float z=0) : position(x,y,z), rotation(0,0,0), scale(1,1,1) {}

    matrix4f getTransformMatrix() const {
        matrix4f scale_matrix;
        matrix4f rotation_matrix;
        matrix4f translation_matrix;
        
        scale_matrix.setScale(scale);
        rotation_matrix.setRotationDegrees(rotation);
        translation_matrix.setTranslation(position);
        
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
    vector3f velocity;
    vector3f force;
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
    vector3f ground_normal;
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
    vector3f position_offset;
    bool visible_only_freecam = true;
};