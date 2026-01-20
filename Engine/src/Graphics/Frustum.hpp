#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <limits>

// Axis-Aligned Bounding Box
struct AABB
{
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    AABB() = default;

    AABB(const glm::vec3& min_pt, const glm::vec3& max_pt)
        : min(min_pt), max(max_pt) {}

    // Get center of the AABB
    glm::vec3 getCenter() const
    {
        return (min + max) * 0.5f;
    }

    // Get half-extents (size from center)
    glm::vec3 getHalfExtents() const
    {
        return (max - min) * 0.5f;
    }

    // Get full size of the AABB
    glm::vec3 getSize() const
    {
        return max - min;
    }

    // Expand this AABB to include a point
    void expand(const glm::vec3& point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    // Expand this AABB to include another AABB
    void expand(const AABB& other)
    {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    // Check if this AABB is valid (has been initialized with points)
    bool isValid() const
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    // Reset to invalid state
    void reset()
    {
        min = glm::vec3(std::numeric_limits<float>::max());
        max = glm::vec3(std::numeric_limits<float>::lowest());
    }

    // Transform a local-space AABB by a model matrix to get world-space AABB
    // This transforms all 8 corners and computes a new axis-aligned bounding box
    static AABB fromTransformedAABB(const glm::vec3& localMin, const glm::vec3& localMax, const glm::mat4& transform)
    {
        AABB result;

        // Get the 8 corners of the local AABB
        glm::vec3 corners[8] = {
            glm::vec3(localMin.x, localMin.y, localMin.z),
            glm::vec3(localMax.x, localMin.y, localMin.z),
            glm::vec3(localMin.x, localMax.y, localMin.z),
            glm::vec3(localMax.x, localMax.y, localMin.z),
            glm::vec3(localMin.x, localMin.y, localMax.z),
            glm::vec3(localMax.x, localMin.y, localMax.z),
            glm::vec3(localMin.x, localMax.y, localMax.z),
            glm::vec3(localMax.x, localMax.y, localMax.z)
        };

        // Transform each corner and expand the result AABB
        for (int i = 0; i < 8; ++i)
        {
            glm::vec4 transformed = transform * glm::vec4(corners[i], 1.0f);
            result.expand(glm::vec3(transformed));
        }

        return result;
    }
};

// Frustum plane representation
struct Plane
{
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance{0.0f};

    Plane() = default;

    Plane(const glm::vec3& n, float d)
        : normal(n), distance(d) {}

    // Create plane from ax + by + cz + d = 0 form
    void setFromCoefficients(float a, float b, float c, float d)
    {
        float length = glm::length(glm::vec3(a, b, c));
        if (length > 0.0001f)
        {
            float inv_len = 1.0f / length;
            normal = glm::vec3(a, b, c) * inv_len;
            distance = d * inv_len;
        }
    }

    // Signed distance from point to plane (positive = in front, negative = behind)
    float distanceToPoint(const glm::vec3& point) const
    {
        return glm::dot(normal, point) + distance;
    }
};

// View frustum for culling
struct Frustum
{
    // 6 frustum planes: Left, Right, Bottom, Top, Near, Far
    enum PlaneIndex
    {
        PLANE_LEFT = 0,
        PLANE_RIGHT,
        PLANE_BOTTOM,
        PLANE_TOP,
        PLANE_NEAR,
        PLANE_FAR,
        PLANE_COUNT
    };

    Plane planes[PLANE_COUNT];

    Frustum() = default;

    // Extract frustum planes from a view-projection matrix using Gribb/Hartmann method
    void extractFromViewProjection(const glm::mat4& vp)
    {
        // Get rows of the matrix (GLM uses column-major, so we access columns)
        glm::vec4 row0 = glm::vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
        glm::vec4 row1 = glm::vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
        glm::vec4 row2 = glm::vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
        glm::vec4 row3 = glm::vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

        // Left plane: row3 + row0
        glm::vec4 left = row3 + row0;
        planes[PLANE_LEFT].setFromCoefficients(left.x, left.y, left.z, left.w);

        // Right plane: row3 - row0
        glm::vec4 right = row3 - row0;
        planes[PLANE_RIGHT].setFromCoefficients(right.x, right.y, right.z, right.w);

        // Bottom plane: row3 + row1
        glm::vec4 bottom = row3 + row1;
        planes[PLANE_BOTTOM].setFromCoefficients(bottom.x, bottom.y, bottom.z, bottom.w);

        // Top plane: row3 - row1
        glm::vec4 top = row3 - row1;
        planes[PLANE_TOP].setFromCoefficients(top.x, top.y, top.z, top.w);

        // Near plane: row3 + row2
        glm::vec4 near_p = row3 + row2;
        planes[PLANE_NEAR].setFromCoefficients(near_p.x, near_p.y, near_p.z, near_p.w);

        // Far plane: row3 - row2
        glm::vec4 far_p = row3 - row2;
        planes[PLANE_FAR].setFromCoefficients(far_p.x, far_p.y, far_p.z, far_p.w);
    }

    // Test if an AABB intersects or is inside the frustum using p-vertex method
    // Returns true if the AABB is at least partially inside the frustum
    bool intersectsAABB(const AABB& aabb) const
    {
        for (int i = 0; i < PLANE_COUNT; ++i)
        {
            const Plane& plane = planes[i];

            // Find the p-vertex (the corner furthest along the plane normal)
            glm::vec3 pVertex;
            pVertex.x = (plane.normal.x >= 0) ? aabb.max.x : aabb.min.x;
            pVertex.y = (plane.normal.y >= 0) ? aabb.max.y : aabb.min.y;
            pVertex.z = (plane.normal.z >= 0) ? aabb.max.z : aabb.min.z;

            // If the p-vertex is outside this plane, the AABB is completely outside the frustum
            if (plane.distanceToPoint(pVertex) < 0)
            {
                return false;
            }
        }

        // AABB is at least partially inside all planes
        return true;
    }

    // Test if an AABB is completely inside the frustum
    bool containsAABB(const AABB& aabb) const
    {
        for (int i = 0; i < PLANE_COUNT; ++i)
        {
            const Plane& plane = planes[i];

            // Find the n-vertex (the corner closest along the plane normal)
            glm::vec3 nVertex;
            nVertex.x = (plane.normal.x >= 0) ? aabb.min.x : aabb.max.x;
            nVertex.y = (plane.normal.y >= 0) ? aabb.min.y : aabb.max.y;
            nVertex.z = (plane.normal.z >= 0) ? aabb.min.z : aabb.max.z;

            // If the n-vertex is outside this plane, the AABB is not completely inside
            if (plane.distanceToPoint(nVertex) < 0)
            {
                return false;
            }
        }

        return true;
    }

    // Test if a point is inside the frustum
    bool containsPoint(const glm::vec3& point) const
    {
        for (int i = 0; i < PLANE_COUNT; ++i)
        {
            if (planes[i].distanceToPoint(point) < 0)
            {
                return false;
            }
        }
        return true;
    }
};
