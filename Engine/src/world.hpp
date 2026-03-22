#pragma once

#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "PhysicsSystem.hpp"
#include <vector>
#include <memory>
#include <entt/entt.hpp>

using namespace std;

class world
{
private:
    std::unique_ptr<PhysicsSystem> physics_system;

public:
    entt::registry registry;
    camera world_camera;
    float fixed_delta;

    world()
    {
        world_camera = camera(0, 0, -5);
        fixed_delta = 1.0f / 60.0f;
        physics_system = std::make_unique<PhysicsSystem>(glm::vec3(0, -1, 0), fixed_delta);
    }

    // Initialize Jolt physics (call after world construction)
    void initializePhysics()
    {
        physics_system->initialize();
    }

    // Physics system access
    PhysicsSystem& getPhysicsSystem() { return *physics_system; }
    const PhysicsSystem& getPhysicsSystem() const { return *physics_system; }

    // Simplified interface that delegates to physics system
    void step_physics()
    {
        physics_system->stepPhysics(registry);
    }

    void player_collisions(entt::entity playerEntity, float sphereRadius)
    {
        physics_system->handlePlayerCollisions(registry, playerEntity, sphereRadius);
    }

    // Utility methods for common physics queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float max_distance,
        glm::vec3& hit_point, glm::vec3& hit_normal)
    {
        return physics_system->raycast(origin, direction, max_distance, registry, hit_point, hit_normal);
    }

    // Configuration methods
    void setGravity(const glm::vec3& gravity)
    {
        physics_system->setGravity(gravity);
    }

    glm::vec3 getGravity() const
    {
        return physics_system->getGravity();
    }

    void setFixedDelta(float delta)
    {
        fixed_delta = delta;
        physics_system->setFixedDelta(delta);
    }
};
