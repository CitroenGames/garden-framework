#include "AnimationSystem.hpp"

#include "Animation/AnimationSampling.hpp"
#include "Components/AnimPlaybackComponent.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Components/IKComponent.hpp"
#include "Components/SkeletonComponent.hpp"
#include "IKSolver.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <algorithm>
#include <cmath>

namespace
{
void seedPlayback(const SkeletonComponent& skeleton, AnimPlaybackComponent& playback)
{
    if (playback.initialized)
        return;

    playback.initialized = true;
    playback.speed = skeleton.playback_speed;
    playback.looping = skeleton.loop;
    if (!skeleton.auto_play || skeleton.default_clip.empty())
        return;

    if (!skeleton.resources)
        return;
    playback.current_clip = skeleton.resources->findClip(skeleton.default_clip);
    if (!playback.current_clip)
        return;
    playback.time = 0.0f;
    playback.playing = true;
    playback.completion_reported = false;
}

bool canAdvance(float dt, float speed)
{
    return std::isfinite(dt) && std::isfinite(speed) && dt >= 0.0f && speed >= 0.0f;
}

bool advancePlayback(AnimPlaybackComponent& playback, float dt, std::string& completed_clip)
{
    completed_clip.clear();
    if (!playback.playing || !playback.current_clip)
        return false;
    if (!canAdvance(dt, playback.speed))
        return true;

    const AnimationClip& clip = *playback.current_clip;
    if (clip.duration <= 0.0f || !std::isfinite(clip.duration))
        return true;

    playback.time += dt * playback.speed;
    if (playback.time < clip.duration)
        return true;

    if (playback.looping)
    {
        playback.time = std::fmod(playback.time, clip.duration);
        return true;
    }

    playback.time = clip.duration;
    playback.playing = false;
    if (!playback.completion_reported)
    {
        completed_clip = clip.name;
        playback.completion_reported = true;
    }
    return false;
}

void applyBlend(const Skeleton& skeleton, AnimPlaybackComponent& playback, float dt, Pose& in_out_pose)
{
    if (!playback.blend_from_clip || playback.blend_duration <= 0.0f || !canAdvance(dt, playback.speed))
        return;

    playback.blend_elapsed += dt;
    const float factor = std::min(playback.blend_elapsed / playback.blend_duration, 1.0f);
    Pose previous_pose;
    AnimationSampling::makeRestPose(skeleton, previous_pose);
    float previous_time = playback.blend_from_time + playback.blend_elapsed * playback.speed;
    if (playback.blend_from_clip->duration > 0.0f && previous_time > playback.blend_from_clip->duration)
        previous_time = std::fmod(previous_time, playback.blend_from_clip->duration);
    AnimationSampling::overlayClip(*playback.blend_from_clip, previous_time, previous_pose);
    in_out_pose = Pose::blend(previous_pose, in_out_pose, factor);
    if (factor >= 1.0f)
        playback.blend_from_clip.reset();
}
} // namespace

namespace AnimationSystem
{
void update(entt::registry& registry, float dt, bool build_skinning_palette)
{
    auto view = registry.view<SkeletonComponent>();
    for (const entt::entity entity : view)
    {
        auto& skeleton_binding = view.get<SkeletonComponent>(entity);
        auto& playback = registry.get_or_emplace<AnimPlaybackComponent>(entity);
        auto& pose = registry.get_or_emplace<AnimPoseComponent>(entity);
        seedPlayback(skeleton_binding, playback);

        const Skeleton* skeleton = skeleton_binding.resources ? skeleton_binding.resources->skeleton() : nullptr;
        if (!skeleton || skeleton->getBoneCount() <= 0)
        {
            pose.local_pose.resize(0);
            pose.bone_matrices.clear();
            pose.pose_valid = false;
            continue;
        }

        AnimationSampling::makeRestPose(*skeleton, pose.local_pose);
        std::string completed_clip;
        advancePlayback(playback, dt, completed_clip);
        if (playback.playing && playback.current_clip)
            AnimationSampling::overlayClip(*playback.current_clip, playback.time, pose.local_pose);
        applyBlend(*skeleton, playback, dt, pose.local_pose);

        if (auto* ik = registry.try_get<IKComponent>(entity); ik && ik->enabled)
        {
            std::vector<glm::mat4> global_transforms;
            skeleton->computeGlobalTransforms(pose.local_pose, global_transforms);
            for (const TwoBoneIKConstraint& constraint : ik->two_bone_constraints)
                TwoBoneIKConstraint::solve(*skeleton, pose.local_pose, global_transforms, constraint);
            for (const FABRIKConstraint& constraint : ik->fabrik_constraints)
                FABRIKConstraint::solve(*skeleton, pose.local_pose, global_transforms, constraint);
        }

        pose.pose_valid = true;
        if (build_skinning_palette)
            skeleton->computeFinalMatrices(pose.local_pose, pose.bone_matrices);
        else
            pose.bone_matrices.clear();

        if (!completed_clip.empty())
            EventBus::get().queue(AnimationFinishedEvent{entity, completed_clip});
    }
}
} // namespace AnimationSystem