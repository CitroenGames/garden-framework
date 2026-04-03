#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>

struct PhysicsDebugConfig
{
    bool enabled = true;

    bool show_collider_aabb   = true;
    bool show_player_capsules = true;
    bool show_velocity        = true;
    bool show_rigidbody_aabb  = true;

    glm::vec3 collider_color   = {0.0f, 0.8f, 0.2f};  // green
    glm::vec3 capsule_color    = {0.0f, 0.8f, 0.8f};  // cyan
    glm::vec3 velocity_color   = {1.0f, 0.6f, 0.0f};  // orange
    glm::vec3 rigidbody_color  = {1.0f, 1.0f, 0.0f};  // yellow

    float velocity_scale = 1.0f;
};

class PhysicsDebugPanel
{
public:
    entt::registry* registry = nullptr;

    void draw();
    void drawDebugVisualization();

private:
    PhysicsDebugConfig m_config;
};
