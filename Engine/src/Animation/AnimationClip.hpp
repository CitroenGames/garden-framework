#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

class Pose;
class AnimationResourceBuilder;

struct PositionKey { float time; glm::vec3 value; };
struct RotationKey { float time; glm::quat value; };
struct ScaleKey { float time; glm::vec3 value; };

struct BoneAnimation
{
    int bone_index = -1;
    std::string bone_name;
    std::vector<PositionKey> position_keys;
    std::vector<RotationKey> rotation_keys;
    std::vector<ScaleKey> scale_keys;

    glm::vec3 samplePosition(float t) const;
    glm::quat sampleRotation(float t) const;
    glm::vec3 sampleScale(float t) const;
};

class AnimationClip
{
public:
    const std::string& name() const { return m_name; }
    float duration() const { return m_duration; }
    float ticksPerSecond() const { return m_ticks_per_second; }
    const std::vector<BoneAnimation>& channels() const { return m_channels; }

    void sample(float time, int bone_count, std::vector<glm::mat4>& out_local_poses) const;
    void samplePose(float time, int bone_count, Pose& out_pose) const;

private:
    friend class AnimationResourceBuilder;

    AnimationClip(std::string name, float duration, float ticks_per_second,
                  std::vector<BoneAnimation> channels)
        : m_name(std::move(name)), m_duration(duration), m_ticks_per_second(ticks_per_second),
          m_channels(std::move(channels)) {}

    std::string m_name;
    float m_duration = 0.0f;
    float m_ticks_per_second = 25.0f;
    std::vector<BoneAnimation> m_channels;
};
