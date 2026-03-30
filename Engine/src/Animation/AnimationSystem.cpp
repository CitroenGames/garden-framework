#include "AnimationSystem.hpp"
#include "Components/AnimationComponent.hpp"
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

        // Advance blender and get local poses
        std::vector<glm::mat4> local_poses;
        anim.blender.update(dt, bone_count, local_poses);

        // Compute final bone matrices from skeleton hierarchy
        anim.skeleton->computeFinalMatrices(local_poses, anim.bone_matrices);

        // Publish event if animation just finished
        if (was_playing && !anim.blender.isPlaying() && clip_before)
        {
            EventBus::get().queue(AnimationFinishedEvent{entity, clip_before->name});
        }
    }
}

} // namespace AnimationSystem
