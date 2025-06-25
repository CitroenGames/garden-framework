#pragma once

#include "irrlicht/vector3.h"
#include "camera.hpp"
#include "gameObject.hpp"
#include "rigidbody.hpp"
#include "collider.hpp"
#include "playerEntity.hpp"
#include "PhysicsSystem.hpp"
#include <vector>

using namespace irr;
using namespace core;
using namespace std;

class world
{
private:
    PhysicsSystem physics_system;

public:
    camera world_camera;
    playerEntity* player_entity;
    float fixed_delta; // Keep for compatibility, but physics system has its own

    world()
    {
        world_camera = camera::camera(0, 0, -5);
        fixed_delta = 0.16f;
        physics_system = PhysicsSystem(vector3f(0, -1, 0), fixed_delta);
        player_entity = nullptr;
    }

    // Physics system access
    PhysicsSystem& getPhysicsSystem() { return physics_system; }
    const PhysicsSystem& getPhysicsSystem() const { return physics_system; }

    // Simplified interface that delegates to physics system
    void step_physics(vector<rigidbody*>& rigidbodies)
    {
        physics_system.stepPhysics(rigidbodies);
    }

    void player_collisions(rigidbody& player_rb, float sphere_radius, std::vector<collider*>& colliders)
    {
        physics_system.handlePlayerCollisions(player_rb, sphere_radius, colliders, player_entity);
    }

    // Utility methods for common physics queries
    bool raycast(const vector3f& origin, const vector3f& direction, float max_distance,
        std::vector<collider*>& colliders, vector3f& hit_point, vector3f& hit_normal)
    {
        return physics_system.raycast(origin, direction, max_distance, colliders, hit_point, hit_normal);
    }

    bool spherecast(const vector3f& origin, float radius, const vector3f& direction,
        float max_distance, std::vector<collider*>& colliders,
        vector3f& hit_point, vector3f& hit_normal)
    {
        return physics_system.spherecast(origin, radius, direction, max_distance, colliders, hit_point, hit_normal);
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