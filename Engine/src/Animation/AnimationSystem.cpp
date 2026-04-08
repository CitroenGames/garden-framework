#include "AnimationSystem.hpp"
#include "Components/AnimationComponent.hpp"
#include "Components/IKComponent.hpp"
#include "Pose.hpp"
#include "IKSolver.hpp"
#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

namespace AnimationSystem
{

void update(entt::registry& registry, float dt)
{
    auto view = registry.view<AnimationComponent>();

    for (auto entity : view)
    {
        auto& anim = view.get<AnimationComponent>(entity);

        if (!anim.skeleton || !anim.blender.isPlaying())
            continue;

        int bone_count = anim.skeleton->getBoneCount();
        if (bone_count <= 0) continue;

        bool was_playing = anim.blender.isPlaying();
        const AnimationClip* clip_before = anim.blender.getCurrentClip();

        // Step 1: Advance blender and produce decomposed pose
        Pose local_pose(bone_count);
        anim.blender.updatePose(dt, bone_count, local_pose);

        // Step 2: Apply IK if entity has IKComponent
        auto* ik = registry.try_get<IKComponent>(entity);
        if (ik && ik->enabled)
        {
            std::vector<glm::mat4> global_transforms;
            anim.skeleton->computeGlobalTransforms(local_pose, global_transforms);

            for (auto& constraint : ik->two_bone_constraints)
            {
                TwoBoneIKConstraint::solve(*anim.skeleton, local_pose, global_transforms, constraint);
            }

            for (auto& constraint : ik->fabrik_constraints)
            {
                FABRIKConstraint::solve(*anim.skeleton, local_pose, global_transforms, constraint);
            }
        }

        // Step 3: Compute final bone matrices for GPU
        anim.skeleton->computeFinalMatrices(local_pose, anim.bone_matrices);

        // Step 4: Publish event if animation just finished
        if (was_playing && !anim.blender.isPlaying() && clip_before)
        {
            EventBus::get().queue(AnimationFinishedEvent{entity, clip_before->name});
        }
    }
}

} // namespace AnimationSystem
