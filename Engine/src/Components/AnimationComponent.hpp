#pragma once

#include "Animation/Skeleton.hpp"
#include "Animation/AnimationClip.hpp"
#include "Animation/AnimationBlender.hpp"
#include "Reflection/Reflector.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

struct AnimationComponent
{
    // Authoring (serialized): which skeleton/clips this entity uses.
    // The loaded Skeleton/clip objects, blender state and bone matrices
    // below are runtime-only and intentionally not reflected.
    std::string skeleton_path;
    std::vector<std::string> clip_names;
    std::string default_clip;
    float playback_speed = 1.0f;
    bool loop = true;
    bool auto_play = false;

    std::shared_ptr<Skeleton> skeleton;
    AnimationBlender blender;

    // Named clips available for this entity
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> clips;

    // Final bone matrices for GPU upload (skeleton space)
    std::vector<glm::mat4> bone_matrices;

    // Convenience: play a named clip
    void playClip(const std::string& name, float blend_time = 0.0f)
    {
        auto it = clips.find(name);
        if (it != clips.end())
        {
            blender.play(it->second, blend_time);
        }
    }

    // Convenience: add a clip by name
    void addClip(const std::string& name, std::shared_ptr<AnimationClip> clip)
    {
        clips[name] = std::move(clip);
    }

    static void reflect(Reflector<AnimationComponent>& r)
    {
        r.display("Animation").category("Animation");
        r.field<&AnimationComponent::skeleton_path>("skeleton_path")
            .tooltip("Skeleton asset path").assetPath().category("Assets");
        r.field<&AnimationComponent::clip_names>("clip_names")
            .tooltip("Names of animation clips available to this entity").category("Clips");
        r.field<&AnimationComponent::default_clip>("default_clip")
            .tooltip("Clip played on spawn when auto_play is set").category("Clips");
        r.field<&AnimationComponent::playback_speed>("playback_speed")
            .tooltip("Playback speed multiplier").drag(0.01f).range(0.0f, 10.0f).category("Playback");
        r.field<&AnimationComponent::loop>("loop")
            .tooltip("Loop the default clip").category("Playback");
        r.field<&AnimationComponent::auto_play>("auto_play")
            .tooltip("Play default_clip automatically").category("Playback");
    }
};
