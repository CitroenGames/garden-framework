#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include "Components/Components.hpp"
#include <entt/entt.hpp>
#include <vector>

struct PhysicsTriangle
{
    glm::vec3 v0, v1, v2;
    glm::vec3 normal;
    glm::vec3 center;
};

class PhysicsSystem
{
private:
    glm::vec3 gravity;
    float fixed_delta;

    // Helper methods
    glm::vec3 calculateSurfaceNormal(const PhysicsTriangle& triangle);
    void transformTriangle(PhysicsTriangle& triangle, const glm::mat4& rotation, const glm::vec3& position);
    void extrudeTriangle(PhysicsTriangle& triangle, float factor = 1.3f);
    bool isPointInsideTriangle(const glm::vec3& point, const PhysicsTriangle& triangle, glm::vec3& barycentricCoords);
    PhysicsTriangle createTriangleFromVertices(const vertex& v0, const vertex& v1, const vertex& v2);

public:
    PhysicsSystem(const glm::vec3& gravityVector = glm::vec3(0, -1, 0), float deltaTime = 0.16f);
    ~PhysicsSystem() = default;

    // Getters and setters
    void setGravity(const glm::vec3& gravityVector) { gravity = gravityVector; }
    glm::vec3 getGravity() const { return gravity; }

    void setFixedDelta(float deltaTime) { fixed_delta = deltaTime; }
    float getFixedDelta() const { return fixed_delta; }

    // Main physics update
    void stepPhysics(entt::registry& registry);

    // Collision detection and response
    void handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity, float sphereRadius);

    // Sphere-triangle collision detection
    bool checkSphereTriangleCollision(const glm::vec3& sphereCenter, float sphereRadius,
        const PhysicsTriangle& triangle, glm::vec3& collisionNormal,
        float& penetrationDepth);

    // General collision queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal);
};
