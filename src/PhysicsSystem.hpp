#pragma once

#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "rigidbody.hpp"
#include "collider.hpp"
#include "mesh.hpp"
#include <vector>

using namespace irr;
using namespace core;

// Forward declaration
class playerEntity;

struct PhysicsTriangle
{
    vector3f v0, v1, v2;
    vector3f normal;
    vector3f center;
};

class PhysicsSystem
{
private:
    vector3f gravity;
    float fixed_delta;

    // Helper methods
    vector3f calculateSurfaceNormal(const PhysicsTriangle& triangle);
    void transformTriangle(PhysicsTriangle& triangle, const matrix4f& rotation, const vector3f& position);
    void extrudeTriangle(PhysicsTriangle& triangle, float factor = 1.3f);
    bool isPointInsideTriangle(const vector3f& point, const PhysicsTriangle& triangle, vector3f& barycentricCoords);
    PhysicsTriangle createTriangleFromVertices(const vertex& v0, const vertex& v1, const vertex& v2);

public:
    PhysicsSystem(const vector3f& gravityVector = vector3f(0, -1, 0), float deltaTime = 0.16f);
    ~PhysicsSystem() = default;

    // Getters and setters
    void setGravity(const vector3f& gravityVector) { gravity = gravityVector; }
    vector3f getGravity() const { return gravity; }

    void setFixedDelta(float deltaTime) { fixed_delta = deltaTime; }
    float getFixedDelta() const { return fixed_delta; }

    // Main physics update
    void stepPhysics(std::vector<rigidbody*>& rigidbodies);

    // Collision detection and response
    void handlePlayerCollisions(rigidbody& playerRigidbody, float sphereRadius,
        std::vector<collider*>& colliders, playerEntity* player);

    // Sphere-triangle collision detection
    bool checkSphereTriangleCollision(const vector3f& sphereCenter, float sphereRadius,
        const PhysicsTriangle& triangle, vector3f& collisionNormal,
        float& penetrationDepth);

    // General collision queries
    bool raycast(const vector3f& origin, const vector3f& direction, float maxDistance,
        std::vector<collider*>& colliders, vector3f& hitPoint, vector3f& hitNormal);

    bool spherecast(const vector3f& origin, float radius, const vector3f& direction,
        float maxDistance, std::vector<collider*>& colliders,
        vector3f& hitPoint, vector3f& hitNormal);
};