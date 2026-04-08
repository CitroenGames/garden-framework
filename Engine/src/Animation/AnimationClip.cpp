#include "AnimationClip.hpp"
#include "Pose.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

// Helper: find the keyframe pair surrounding time t
template<typename KeyType>
static std::pair<int, int> findKeyPair(const std::vector<KeyType>& keys, float t)
{
    if (keys.empty()) return {-1, -1};
    if (keys.size() == 1 || t <= keys.front().time) return {0, 0};
    if (t >= keys.back().time) return {static_cast<int>(keys.size()) - 1, static_cast<int>(keys.size()) - 1};

    for (int i = 0; i < static_cast<int>(keys.size()) - 1; i++)
    {
        if (t >= keys[i].time && t < keys[i + 1].time)
        {
            return {i, i + 1};
        }
    }
    return {static_cast<int>(keys.size()) - 1, static_cast<int>(keys.size()) - 1};
}

static float getFactor(float t, float t0, float t1)
{
    if (t1 <= t0) return 0.0f;
    return (t - t0) / (t1 - t0);
}

glm::vec3 BoneAnimation::samplePosition(float t) const
{
    if (position_keys.empty()) return glm::vec3(0.0f);
    if (position_keys.size() == 1) return position_keys[0].value;

    auto [i0, i1] = findKeyPair(position_keys, t);
    if (i0 == i1) return position_keys[i0].value;

    float factor = getFactor(t, position_keys[i0].time, position_keys[i1].time);
    return glm::mix(position_keys[i0].value, position_keys[i1].value, factor);
}

glm::quat BoneAnimation::sampleRotation(float t) const
{
    if (rotation_keys.empty()) return glm::quat(1, 0, 0, 0);
    if (rotation_keys.size() == 1) return rotation_keys[0].value;

    auto [i0, i1] = findKeyPair(rotation_keys, t);
    if (i0 == i1) return rotation_keys[i0].value;

    float factor = getFactor(t, rotation_keys[i0].time, rotation_keys[i1].time);
    return glm::slerp(rotation_keys[i0].value, rotation_keys[i1].value, factor);
}

glm::vec3 BoneAnimation::sampleScale(float t) const
{
    if (scale_keys.empty()) return glm::vec3(1.0f);
    if (scale_keys.size() == 1) return scale_keys[0].value;

    auto [i0, i1] = findKeyPair(scale_keys, t);
    if (i0 == i1) return scale_keys[i0].value;

    float factor = getFactor(t, scale_keys[i0].time, scale_keys[i1].time);
    return glm::mix(scale_keys[i0].value, scale_keys[i1].value, factor);
}

void AnimationClip::sample(float time, int bone_count, std::vector<glm::mat4>& out_local_poses) const
{
    out_local_poses.resize(bone_count, glm::mat4(1.0f));

    for (const auto& channel : channels)
    {
        if (channel.bone_index < 0 || channel.bone_index >= bone_count) continue;

        glm::vec3 pos = channel.samplePosition(time);
        glm::quat rot = channel.sampleRotation(time);
        glm::vec3 scl = channel.sampleScale(time);

        glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
        glm::mat4 r = glm::toMat4(rot);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scl);

        out_local_poses[channel.bone_index] = t * r * s;
    }
}

void AnimationClip::samplePose(float time, int bone_count, Pose& out_pose) const
{
    out_pose.resize(bone_count);

    // Initialize all bones to identity
    for (int i = 0; i < bone_count; i++)
    {
        out_pose[i] = BonePose::identity();
    }

    for (const auto& channel : channels)
    {
        if (channel.bone_index < 0 || channel.bone_index >= bone_count) continue;

        out_pose[channel.bone_index].translation = channel.samplePosition(time);
        out_pose[channel.bone_index].rotation = channel.sampleRotation(time);
        out_pose[channel.bone_index].scale = channel.sampleScale(time);
    }
}
