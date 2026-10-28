#include "AnimationSampling.hpp"

#include "AnimationClip.hpp"
#include "Pose.hpp"

#include <vector>

namespace AnimationSampling
{
void makeRestPose(const Skeleton& skeleton, Pose& out_pose)
{
    std::vector<glm::mat4> rest_matrices;
    rest_matrices.reserve(static_cast<size_t>(skeleton.getBoneCount()));
    for (const Bone& bone : skeleton.getBones())
        rest_matrices.push_back(bone.local_transform);
    out_pose.fromMatrices(rest_matrices);
}

void overlayClip(const AnimationClip& clip, float time, Pose& in_out_pose)
{
    const int bone_count = in_out_pose.getBoneCount();
    for (const BoneAnimation& channel : clip.channels())
    {
        if (channel.bone_index < 0 || channel.bone_index >= bone_count)
            continue;

        BonePose& bone = in_out_pose[channel.bone_index];
        if (!channel.position_keys.empty())
            bone.translation = channel.samplePosition(time);
        if (!channel.rotation_keys.empty())
            bone.rotation = channel.sampleRotation(time);
        if (!channel.scale_keys.empty())
            bone.scale = channel.sampleScale(time);
    }
}
} // namespace AnimationSampling
