#pragma once

#include "Components/camera.hpp"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

struct PreviewOrbitCamera
{
    float yaw = glm::radians(225.0f);
    float pitch = 0.3f;
    float distance = 3.0f;
    glm::vec3 target{0.0f};

    float min_distance = 0.1f;
    float max_distance = 100.0f;
    float orbit_speed = 0.005f;
    float zoom_speed = 0.1f;
    float auto_rotate_speed = 0.3f;

    camera toCamera() const
    {
        float x = distance * cosf(pitch) * sinf(yaw);
        float y = distance * sinf(pitch);
        float z = distance * cosf(pitch) * cosf(yaw);

        camera cam;
        cam.position = target + glm::vec3(x, y, z);

        // Compute forward direction toward target
        glm::vec3 forward = glm::normalize(target - cam.position);

        // Extract rotation matching the engine's camera convention:
        // camera_forward() = quat(vec3(pitch, yaw, 0)) * vec3(0,0,1)
        // With RH lookAt, we extract from the forward vector
        cam.rotation.x = asinf(glm::clamp(forward.y, -1.0f, 1.0f));
        cam.rotation.y = atan2f(forward.x, forward.z);
        cam.rotation.z = 0.0f;

        return cam;
    }

    void orbit(float dx_pixels, float dy_pixels)
    {
        yaw -= dx_pixels * orbit_speed;
        pitch += dy_pixels * orbit_speed;

        // Clamp pitch to avoid gimbal lock
        float limit = glm::radians(89.0f);
        pitch = std::clamp(pitch, -limit, limit);
    }

    void zoom(float scroll_delta)
    {
        distance -= scroll_delta * distance * zoom_speed;
        distance = std::clamp(distance, min_distance, max_distance);
    }

    void frameAABB(glm::vec3 aabb_min, glm::vec3 aabb_max, float fov_radians)
    {
        target = (aabb_min + aabb_max) * 0.5f;
        float radius = glm::length(aabb_max - aabb_min) * 0.5f;
        if (radius < 0.001f) radius = 1.0f;
        distance = radius / sinf(fov_radians * 0.5f);
        distance *= 1.3f;
        min_distance = radius * 0.1f;
        max_distance = distance * 10.0f;
        pitch = 0.3f;
        yaw = glm::radians(225.0f);
    }
};
