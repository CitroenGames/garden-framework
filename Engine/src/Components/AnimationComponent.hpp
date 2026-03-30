#pragma once

#include "Animation/Skeleton.hpp"
#include "Animation/AnimationClip.hpp"
#include "Animation/AnimationBlender.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

struct AnimationComponent
{
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
};
