#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

class camera
{
public:
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale; // Kept for consistency, though usually 1,1,1 for camera

    camera(float x = 0, float y = 0, float z = 0)
    {
        position = glm::vec3(x, y, z);
        rotation = glm::vec3(0, 0, 0);
        scale = glm::vec3(1, 1, 1);
    }

    glm::quat camera_rot_quaternion() const
    {
        glm::vec3 clamped_rotation = rotation;
        clamped_rotation.x = glm::clamp(clamped_rotation.x, -1.0f, 1.0f);
        return glm::quat(clamped_rotation);
    }

    glm::vec3 camera_forward() const
    {
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, 1.0f);
        return camera_rot_quaternion() * forward;
    }

    // Get the view matrix for this camera
    glm::mat4 getViewMatrix() const
    {
        glm::vec3 forward = camera_forward();
        glm::vec3 target = position + forward;
        glm::vec3 up = glm::vec3(0, 1, 0);

        return glm::lookAtLH(position, target, up);
    }

    // Get camera properties for render API
    glm::vec3 getPosition() const { return position; }
    glm::vec3 getTarget() const { return position + camera_forward(); }
    glm::vec3 getUpVector() const { return glm::vec3(0, 1, 0); }
};