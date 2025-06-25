#include "PhysicsSystem.hpp"
#include "playerEntity.hpp"
#include <stdio.h>
#include <cmath>

PhysicsSystem::PhysicsSystem(const vector3f& gravityVector, float deltaTime)
    : gravity(gravityVector), fixed_delta(deltaTime)
{
}

void PhysicsSystem::stepPhysics(std::vector<rigidbody*>& rigidbodies)
{
    // Explicit Euler integration for all rigidbodies
    if (!rigidbodies.empty())
    {
        for (auto& rb : rigidbodies)
        {
            if (!rb) continue;

            // Apply gravity if enabled
            if (rb->apply_gravity)
                rb->force += gravity;

            // Integrate velocity
            rb->velocity += rb->force * fixed_delta;

            // Integrate position
            rb->obj.position += rb->velocity * fixed_delta;

            // Reset forces for next frame
            rb->force = vector3f(0, 0, 0);
        }
    }
}

void PhysicsSystem::handlePlayerCollisions(rigidbody& playerRigidbody, float sphereRadius,
    std::vector<collider*>& colliders, playerEntity* player)
{
    if (!player) return;

    // Reset ground state
    player->update_grounded(false);
    player->update_ground_normal(vector3f(0, 1, 0));

    if (colliders.empty()) return;

    vector3f sphereCenter = playerRigidbody.obj.position;

    // Check collision with each collider
    for (auto& collider : colliders)
    {
        if (!collider || !collider->is_mesh_valid())
            continue;

        mesh* colliderMesh = collider->get_mesh();
        if (!colliderMesh)
            continue;

        // Check collision with each triangle in the mesh
        for (size_t i = 0; i < colliderMesh->vertices_len; i += 3)
        {
            if (i + 2 >= colliderMesh->vertices_len)
                break;

            // Create triangle from vertices
            PhysicsTriangle triangle = createTriangleFromVertices(
                colliderMesh->vertices[i],
                colliderMesh->vertices[i + 1],
                colliderMesh->vertices[i + 2]
            );

            // Transform triangle to world space
            transformTriangle(triangle, collider->obj.getRotationMatrix(), collider->obj.position);

            // Extrude triangle slightly for better collision detection
            extrudeTriangle(triangle);

            // Check if sphere is facing the triangle
            vector3f dirToSphere = sphereCenter - triangle.center;
            dirToSphere.normalize();
            float facingDot = triangle.normal.dotProduct(dirToSphere);

            if (facingDot <= 0)
                continue; // Sphere is behind the triangle

            // Check for collision
            vector3f collisionNormal;
            float penetrationDepth;
            if (checkSphereTriangleCollision(sphereCenter, sphereRadius, triangle,
                collisionNormal, penetrationDepth))
            {
                printf("Collision detected! Penetration: %f\n", penetrationDepth);

                // Resolve collision by moving sphere out of triangle
                playerRigidbody.obj.position += collisionNormal * penetrationDepth;

                // Update ground state if this surface can be considered ground
                vector3f gravityNormal = -gravity;
                gravityNormal.normalize();
                if (triangle.normal.dotProduct(gravityNormal) > 0.5f) // Angle threshold for "ground"
                {
                    player->update_grounded(true);
                    player->update_ground_normal(triangle.normal);
                }
            }
        }
    }
}

bool PhysicsSystem::checkSphereTriangleCollision(const vector3f& sphereCenter, float sphereRadius,
    const PhysicsTriangle& triangle, vector3f& collisionNormal,
    float& penetrationDepth)
{
    // Project sphere center onto triangle plane
    vector3f toCenter = sphereCenter - triangle.center;
    float distanceToPlane = triangle.normal.dotProduct(toCenter);
    vector3f projectedPoint = sphereCenter - triangle.normal * distanceToPlane;

    // Check if projected point is inside triangle
    vector3f barycentricCoords;
    if (!isPointInsideTriangle(projectedPoint, triangle, barycentricCoords))
        return false;

    // Check if sphere intersects with triangle
    float distanceFromCollision = std::abs(distanceToPlane);
    if (distanceFromCollision > sphereRadius)
        return false;

    // We have a collision
    collisionNormal = triangle.normal;
    if (distanceToPlane < 0)
        collisionNormal = -collisionNormal; // Flip normal if sphere is on wrong side

    penetrationDepth = sphereRadius - distanceFromCollision;
    return true;
}

PhysicsTriangle PhysicsSystem::createTriangleFromVertices(const vertex& v0, const vertex& v1, const vertex& v2)
{
    PhysicsTriangle triangle;
    triangle.v0 = vector3f(v0.vx, v0.vy, v0.vz);
    triangle.v1 = vector3f(v1.vx, v1.vy, v1.vz);
    triangle.v2 = vector3f(v2.vx, v2.vy, v2.vz);
    triangle.center = (triangle.v0 + triangle.v1 + triangle.v2) / 3.0f;
    triangle.normal = calculateSurfaceNormal(triangle);
    return triangle;
}

vector3f PhysicsSystem::calculateSurfaceNormal(const PhysicsTriangle& triangle)
{
    vector3f edge1 = triangle.v1 - triangle.v0;
    vector3f edge2 = triangle.v2 - triangle.v0;
    vector3f normal = edge1.crossProduct(edge2);
    normal.normalize();
    return normal;
}

void PhysicsSystem::transformTriangle(PhysicsTriangle& triangle, const matrix4f& rotation, const vector3f& position)
{
    // Apply rotation
    rotation.transformVect(triangle.v0);
    rotation.transformVect(triangle.v1);
    rotation.transformVect(triangle.v2);
    rotation.transformVect(triangle.normal);

    // Apply translation
    triangle.v0 += position;
    triangle.v1 += position;
    triangle.v2 += position;

    // Recalculate center
    triangle.center = (triangle.v0 + triangle.v1 + triangle.v2) / 3.0f;
}

void PhysicsSystem::extrudeTriangle(PhysicsTriangle& triangle, float factor)
{
    triangle.v0 = (triangle.v0 - triangle.center) * factor + triangle.center;
    triangle.v1 = (triangle.v1 - triangle.center) * factor + triangle.center;
    triangle.v2 = (triangle.v2 - triangle.center) * factor + triangle.center;
}

bool PhysicsSystem::isPointInsideTriangle(const vector3f& point, const PhysicsTriangle& triangle,
    vector3f& barycentricCoords)
{
    // Barycentric coordinate calculation
    // Express point as weighted sum of triangle vertices
    vector3f v0 = triangle.v2 - triangle.v0;
    vector3f v1 = triangle.v1 - triangle.v0;
    vector3f v2 = point - triangle.v0;

    float dot00 = v0.dotProduct(v0);
    float dot01 = v0.dotProduct(v1);
    float dot02 = v0.dotProduct(v2);
    float dot11 = v1.dotProduct(v1);
    float dot12 = v1.dotProduct(v2);

    float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
    float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
    float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

    // Store barycentric coordinates
    barycentricCoords = vector3f(1.0f - u - v, v, u);

    // Check if point is inside triangle
    return (u >= 0) && (v >= 0) && (u + v <= 1);
}

bool PhysicsSystem::raycast(const vector3f& origin, const vector3f& direction, float maxDistance,
    std::vector<collider*>& colliders, vector3f& hitPoint, vector3f& hitNormal)
{
    float closestDistance = maxDistance;
    bool hit = false;
    vector3f normalizedDirection = direction;
    normalizedDirection.normalize();

    for (auto& collider : colliders)
    {
        if (!collider || !collider->is_mesh_valid())
            continue;

        mesh* colliderMesh = collider->get_mesh();
        if (!colliderMesh)
            continue;

        // Check ray against each triangle
        for (size_t i = 0; i < colliderMesh->vertices_len; i += 3)
        {
            if (i + 2 >= colliderMesh->vertices_len)
                break;

            PhysicsTriangle triangle = createTriangleFromVertices(
                colliderMesh->vertices[i],
                colliderMesh->vertices[i + 1],
                colliderMesh->vertices[i + 2]
            );

            transformTriangle(triangle, collider->obj.getRotationMatrix(), collider->obj.position);

            // Ray-triangle intersection using Möller-Trumbore algorithm
            vector3f edge1 = triangle.v1 - triangle.v0;
            vector3f edge2 = triangle.v2 - triangle.v0;
            vector3f h = normalizedDirection.crossProduct(edge2);
            float a = edge1.dotProduct(h);

            if (a > -0.00001f && a < 0.00001f)
                continue; // Ray is parallel to triangle

            float f = 1.0f / a;
            vector3f s = origin - triangle.v0;
            float u = f * s.dotProduct(h);

            if (u < 0.0f || u > 1.0f)
                continue;

            vector3f q = s.crossProduct(edge1);
            float v = f * normalizedDirection.dotProduct(q);

            if (v < 0.0f || u + v > 1.0f)
                continue;

            float t = f * edge2.dotProduct(q);

            if (t > 0.00001f && t < closestDistance)
            {
                closestDistance = t;
                hitPoint = origin + normalizedDirection * t;
                hitNormal = triangle.normal;
                hit = true;
            }
        }
    }

    return hit;
}

bool PhysicsSystem::spherecast(const vector3f& origin, float radius, const vector3f& direction,
    float maxDistance, std::vector<collider*>& colliders,
    vector3f& hitPoint, vector3f& hitNormal)
{
    // Simple implementation: perform multiple raycasts around the sphere
    vector3f normalizedDirection = direction;
    normalizedDirection.normalize();

    // Primary raycast along the center
    if (raycast(origin, direction, maxDistance, colliders, hitPoint, hitNormal))
    {
        // Adjust hit point to account for sphere radius
        hitPoint -= normalizedDirection * radius;
        return true;
    }

    // Additional raycasts could be added here for more accurate spherecasting
    // For now, this simple implementation should suffice

    return false;
}