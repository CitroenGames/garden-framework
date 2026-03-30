#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

struct PositionKey
{
    float time;
    glm::vec3 value;
};

struct RotationKey
{
    float time;
    glm::quat value;
};

struct ScaleKey
{
    float time;
    glm::vec3 value;
};

struct BoneAnimation
{
    int bone_index = -1;
    std::string bone_name;

    std::vector<PositionKey> position_keys;
    std::vector<RotationKey> rotation_keys;
    std::vector<ScaleKey> scale_keys;

    // Interpolate position at time t
    glm::vec3 samplePosition(float t) const;

    // Interpolate rotation at time t (SLERP)
    glm::quat sampleRotation(float t) const;

    // Interpolate scale at time t
    glm::vec3 sampleScale(float t) const;
};

class AnimationClip
{
public:
    std::string name;
    float duration = 0.0f;
    float ticks_per_second = 25.0f;

    std::vector<BoneAnimation> channels;

    // Sample all bone local transforms at the given time
    // Returns a vector of local transform matrices indexed by bone index
    void sample(float time, int bone_count, std::vector<glm::mat4>& out_local_poses) const;
};
