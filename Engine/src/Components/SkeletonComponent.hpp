#pragma once

#include "Animation/AnimationResourceSet.hpp"
#include "Reflection/Reflector.hpp"

#include <memory>
#include <string>
#include <vector>

// Reflected authoring binding. The resolved immutable resource set is runtime
// data and is deliberately excluded from level/prefab serialization.
struct SkeletonComponent
{
    std::string skeleton_path;
    std::vector<std::string> clip_names;
    std::string default_clip;
    float playback_speed = 1.0f;
    bool loop = true;
    bool auto_play = false;

    std::shared_ptr<const AnimationResourceSet> resources;

    static void reflect(Reflector<SkeletonComponent>& r)
    {
        r.display("Skeleton").category("Animation");
        r.field<&SkeletonComponent::skeleton_path>("skeleton_path")
            .tooltip("Skeleton asset path").assetPath().category("Assets");
        r.field<&SkeletonComponent::clip_names>("clip_names")
            .tooltip("Names of animation clips available to this entity").category("Clips");
        r.field<&SkeletonComponent::default_clip>("default_clip")
            .tooltip("Clip played on spawn when auto_play is set").category("Clips");
        r.field<&SkeletonComponent::playback_speed>("playback_speed")
            .tooltip("Playback speed multiplier").drag(0.01f).range(0.0f, 10.0f).category("Playback");
        r.field<&SkeletonComponent::loop>("loop")
            .tooltip("Loop the default clip").category("Playback");
        r.field<&SkeletonComponent::auto_play>("auto_play")
            .tooltip("Play default_clip automatically").category("Playback");
    }
};