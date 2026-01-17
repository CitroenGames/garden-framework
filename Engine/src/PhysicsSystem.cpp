#include "PhysicsSystem.hpp"
#include <stdio.h>
#include <cmath>

PhysicsSystem::PhysicsSystem(const glm::vec3& gravityVector, float deltaTime)
    : gravity(gravityVector), fixed_delta(deltaTime)
{
}

void PhysicsSystem::stepPhysics(entt::registry& registry)
{
    auto view = registry.view<RigidBodyComponent, TransformComponent>();
    
    for(auto entity : view) {
        auto& rb = view.get<RigidBodyComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);

        // Apply gravity if enabled
        if (rb.apply_gravity)
            rb.force += gravity;

        // Integrate velocity
        rb.velocity += rb.force * fixed_delta;

        // Integrate position
        transform.position += rb.velocity * fixed_delta;

        // Reset forces for next frame
        rb.force = glm::vec3(0, 0, 0);
    }
}

void PhysicsSystem::handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity, float sphereRadius)
{
    if (!registry.valid(playerEntity)) return;

    // Check if entity has required components
    if (!registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(playerEntity)) return;

    auto& transform = registry.get<TransformComponent>(playerEntity);
    // rb unused here but good to know it exists
    auto& player = registry.get<PlayerComponent>(playerEntity);

    // Reset ground state
    player.grounded = false;
    player.ground_normal = glm::vec3(0, 1, 0);

    glm::vec3 sphereCenter = transform.position;

    // Check collision with each collider
    auto view = registry.view<ColliderComponent, TransformComponent>();
    
    for(auto entity : view) {
        // Skip self
        if (entity == playerEntity) continue;
        
        auto& collider = view.get<ColliderComponent>(entity);
        auto& col_transform = view.get<TransformComponent>(entity);
        
        if (!collider.is_mesh_valid()) continue;

        mesh* colliderMesh = collider.get_mesh();
        
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
            glm::mat4 rotMat = glm::eulerAngleYXZ(
                glm::radians(col_transform.rotation.y),
                glm::radians(col_transform.rotation.x),
                glm::radians(col_transform.rotation.z));
            transformTriangle(triangle, rotMat, col_transform.position);

            // Extrude triangle slightly for better collision detection
            extrudeTriangle(triangle);

            // Check if sphere is facing the triangle
            glm::vec3 dirToSphere = glm::normalize(sphereCenter - triangle.center);
            float facingDot = glm::dot(triangle.normal, dirToSphere);

            if (facingDot <= 0)
                continue; // Sphere is behind the triangle

            glm::vec3 collisionNormal;
            float penetrationDepth;
            if (checkSphereTriangleCollision(sphereCenter, sphereRadius, triangle,
                collisionNormal, penetrationDepth))
            {
                // Resolve collision by moving sphere out of triangle
                transform.position += collisionNormal * penetrationDepth;
                sphereCenter = transform.position;

                // Update ground state if this surface can be considered ground
                glm::vec3 gravityNormal = glm::normalize(-gravity);
                if (glm::dot(triangle.normal, gravityNormal) > 0.5f) // Angle threshold for "ground"
                {
                    player.grounded = true;
                    player.ground_normal = triangle.normal;
                }
            }
        }
    }
}

bool PhysicsSystem::checkSphereTriangleCollision(const glm::vec3& sphereCenter, float sphereRadius,
    const PhysicsTriangle& triangle, glm::vec3& collisionNormal,
    float& penetrationDepth)
{
    // Project sphere center onto triangle plane
    glm::vec3 toCenter = sphereCenter - triangle.center;
    float distanceToPlane = glm::dot(triangle.normal, toCenter);
    glm::vec3 projectedPoint = sphereCenter - triangle.normal * distanceToPlane;

    // Check if projected point is inside triangle
    glm::vec3 barycentricCoords;
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
    triangle.v0 = glm::vec3(v0.vx, v0.vy, v0.vz);
    triangle.v1 = glm::vec3(v1.vx, v1.vy, v1.vz);
    triangle.v2 = glm::vec3(v2.vx, v2.vy, v2.vz);
    triangle.center = (triangle.v0 + triangle.v1 + triangle.v2) / 3.0f;
    triangle.normal = calculateSurfaceNormal(triangle);
    return triangle;
}

glm::vec3 PhysicsSystem::calculateSurfaceNormal(const PhysicsTriangle& triangle)
{
    glm::vec3 edge1 = triangle.v1 - triangle.v0;
    glm::vec3 edge2 = triangle.v2 - triangle.v0;
    return glm::normalize(glm::cross(edge1, edge2));
}

void PhysicsSystem::transformTriangle(PhysicsTriangle& triangle, const glm::mat4& rotation, const glm::vec3& position)
{
    // Apply rotation (use w=1 for positions, w=0 for directions)
    triangle.v0 = glm::vec3(rotation * glm::vec4(triangle.v0, 1.0f));
    triangle.v1 = glm::vec3(rotation * glm::vec4(triangle.v1, 1.0f));
    triangle.v2 = glm::vec3(rotation * glm::vec4(triangle.v2, 1.0f));
    triangle.normal = glm::vec3(rotation * glm::vec4(triangle.normal, 0.0f));

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

bool PhysicsSystem::isPointInsideTriangle(const glm::vec3& point, const PhysicsTriangle& triangle,
    glm::vec3& barycentricCoords)
{
    // Barycentric coordinate calculation
    // Express point as weighted sum of triangle vertices
    glm::vec3 v0 = triangle.v2 - triangle.v0;
    glm::vec3 v1 = triangle.v1 - triangle.v0;
    glm::vec3 v2 = point - triangle.v0;

    float dot00 = glm::dot(v0, v0);
    float dot01 = glm::dot(v0, v1);
    float dot02 = glm::dot(v0, v2);
    float dot11 = glm::dot(v1, v1);
    float dot12 = glm::dot(v1, v2);

    float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
    float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
    float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

    // Store barycentric coordinates
    barycentricCoords = glm::vec3(1.0f - u - v, v, u);

    // Check if point is inside triangle
    return (u >= 0) && (v >= 0) && (u + v <= 1);
}

bool PhysicsSystem::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal)
{
    float closestDistance = maxDistance;
    bool hit = false;
    glm::vec3 normalizedDirection = glm::normalize(direction);

    auto view = registry.view<ColliderComponent, TransformComponent>();

    for (auto entity : view)
    {
        auto& collider = view.get<ColliderComponent>(entity);
        auto& col_transform = view.get<TransformComponent>(entity);

        if (!collider.is_mesh_valid())
            continue;

        mesh* colliderMesh = collider.get_mesh();

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

            glm::mat4 rotMat = glm::eulerAngleYXZ(
                glm::radians(col_transform.rotation.y),
                glm::radians(col_transform.rotation.x),
                glm::radians(col_transform.rotation.z));
            transformTriangle(triangle, rotMat, col_transform.position);

            // Ray-triangle intersection using Möller-Trumbore algorithm
            glm::vec3 edge1 = triangle.v1 - triangle.v0;
            glm::vec3 edge2 = triangle.v2 - triangle.v0;
            glm::vec3 h = glm::cross(normalizedDirection, edge2);
            float a = glm::dot(edge1, h);

            if (a > -0.00001f && a < 0.00001f)
                continue; // Ray is parallel to triangle

            float f = 1.0f / a;
            glm::vec3 s = origin - triangle.v0;
            float u = f * glm::dot(s, h);

            if (u < 0.0f || u > 1.0f)
                continue;

            glm::vec3 q = glm::cross(s, edge1);
            float v = f * glm::dot(normalizedDirection, q);

            if (v < 0.0f || u + v > 1.0f)
                continue;

            float t = f * glm::dot(edge2, q);

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
