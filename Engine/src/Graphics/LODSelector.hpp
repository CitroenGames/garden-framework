#pragma once

#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

class LODSelector {
public:
    // Select LOD level based on screen-space coverage
    // Returns 0 for LOD0 (highest detail), 1 for LOD1, etc.
    // screen_thresholds[i] is the coverage below which LOD i is used (LOD0 threshold is ignored)
    static int selectLOD(const glm::vec3& camera_pos,
                         const glm::vec3& object_pos,
                         const glm::vec3& aabb_min,
                         const glm::vec3& aabb_max,
                         const glm::mat4& projection,
                         int lod_count,
                         const float* screen_thresholds)
    {
        if (lod_count <= 1)
            return 0;

        float coverage = computeScreenCoverage(camera_pos, object_pos, aabb_min, aabb_max, projection);

        // Walk from lowest LOD to highest, find the first LOD whose threshold
        // is less than or equal to the coverage
        for (int i = lod_count - 1; i >= 1; --i)
        {
            if (coverage <= screen_thresholds[i])
                return i;
        }

        return 0; // highest detail
    }

    // Compute approximate screen-space coverage (0.0 to 1.0+)
    // Uses the bounding sphere projected through the projection matrix
    static float computeScreenCoverage(const glm::vec3& camera_pos,
                                        const glm::vec3& object_pos,
                                        const glm::vec3& aabb_min,
                                        const glm::vec3& aabb_max,
                                        const glm::mat4& projection)
    {
        // Bounding sphere radius from AABB
        glm::vec3 extent = (aabb_max - aabb_min) * 0.5f;
        float radius = glm::length(extent);

        if (radius <= 0.0f)
            return 0.0f;

        // Distance from camera to object center
        glm::vec3 center = (aabb_min + aabb_max) * 0.5f + object_pos;
        float distance = glm::length(center - camera_pos);

        if (distance <= radius)
            return 1.0f; // camera is inside or very close

        // Project the bounding sphere to screen space
        // projection[1][1] = 1 / tan(fov_y / 2) = cotangent of half-FOV
        float proj_scale = std::abs(projection[1][1]);

        // Screen coverage = projected_diameter / viewport_height
        // projected_size = (radius / distance) * proj_scale
        // coverage = 2 * projected_size (diameter, not radius)
        float coverage = (2.0f * radius * proj_scale) / distance;

        return coverage;
    }
};
