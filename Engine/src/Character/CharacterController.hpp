#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstdint>

namespace CharacterMoveFlags
{
    constexpr uint8_t Jump = 1 << 0;
}

struct CharacterMoveInput
{
    float move_forward = 0.0f;
    float move_right = 0.0f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    uint8_t buttons = 0;
};

struct CharacterControllerState
{
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 ground_velocity = glm::vec3(0.0f);
};

namespace CharacterController
{
    inline bool wantsJump(const CharacterMoveInput& input)
    {
        return (input.buttons & CharacterMoveFlags::Jump) != 0;
    }

    inline glm::vec3 buildWishDirection(const CharacterMoveInput& input,
                                        bool grounded,
                                        const glm::vec3& ground_normal)
    {
        glm::vec3 wish_dir(input.move_right, 0.0f, input.move_forward);

        const float clamped_pitch = std::clamp(input.camera_pitch, -1.0f, 1.0f);
        const glm::quat camera_rotation = glm::quat(glm::vec3(clamped_pitch, input.camera_yaw, 0.0f));
        wish_dir = camera_rotation * wish_dir;

        if (grounded)
            wish_dir = wish_dir - glm::dot(wish_dir, ground_normal) * ground_normal;
        else
            wish_dir.y = 0.0f;

        if (glm::length(wish_dir) > 0.001f)
            return glm::normalize(wish_dir);

        return glm::vec3(0.0f);
    }
}
