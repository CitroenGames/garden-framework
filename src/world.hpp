#pragma once

#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "PhysicsSystem.hpp"
#include <vector>
#include <entt/entt.hpp>

using namespace irr;
using namespace core;
using namespace std;

class world
{
private:
    PhysicsSystem physics_system;

public:
    entt::registry registry;
    camera world_camera;
    float fixed_delta; 

    world()
    {
        world_camera = camera::camera(0, 0, -5);
        fixed_delta = 0.16f;
        physics_system = PhysicsSystem(vector3f(0, -1, 0), fixed_delta);
    }

    // Physics system access
    PhysicsSystem& getPhysicsSystem() { return physics_system; }
    const PhysicsSystem& getPhysicsSystem() const { return physics_system; }

    // Simplified interface that delegates to physics system
    void step_physics()
    {
        physics_system.stepPhysics(registry);
    }

    void player_collisions(entt::entity playerEntity, float sphereRadius)
    {
        physics_system.handlePlayerCollisions(registry, playerEntity, sphereRadius);
    }

    // Utility methods for common physics queries
    bool raycast(const vector3f& origin, const vector3f& direction, float max_distance,
        vector3f& hit_point, vector3f& hit_normal)
    {
        return physics_system.raycast(origin, direction, max_distance, registry, hit_point, hit_normal);
    }

    // Configuration methods
    void setGravity(const vector3f& gravity)
    {
        physics_system.setGravity(gravity);
    }

    vector3f getGravity() const
    {
        return physics_system.getGravity();
    }

    void setFixedDelta(float delta)
    {
        fixed_delta = delta;
        physics_system.setFixedDelta(delta);
    }
};
