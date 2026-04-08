#pragma once

#include "AnimationClip.hpp"
#include "Pose.hpp"
#include "BoneMask.hpp"
#include <memory>
#include <algorithm>
#include <cmath>

enum class BlendMode
{
    Override, // Layer pose replaces base (weighted by mask)
    Additive  // Layer pose adds to base
};

struct AnimationLayer
{
    std::shared_ptr<AnimationClip> clip;
    std::shared_ptr<AnimationClip> blend_from_clip; // clip we're crossfading from within this layer

    Pose reference_pose; // for additive mode: the neutral/reference pose

    BoneMask mask;
    BlendMode mode = BlendMode::Override;
    float weight = 1.0f; // overall layer weight

    float playback_time = 0.0f;
    float playback_speed = 1.0f;
    bool playing = false;
    bool looping = true;

    // Crossfade state within this layer
    float blend_factor = 1.0f;
    float blend_duration = 0.0f;
    float blend_elapsed = 0.0f;
    float blend_from_time = 0.0f;

    void play(std::shared_ptr<AnimationClip> new_clip, float blend_time = 0.0f)
    {
        if (!new_clip) return;

        if (blend_time > 0.0f && clip && playing)
        {
            blend_from_clip = clip;
            blend_from_time = playback_time;
            blend_duration = blend_time;
            blend_elapsed = 0.0f;
            blend_factor = 0.0f;
        }
        else
        {
            blend_from_clip = nullptr;
            blend_factor = 1.0f;
        }

        clip = std::move(new_clip);
        playback_time = 0.0f;
        playing = true;
    }

    void stop()
    {
        playing = false;
        blend_from_clip = nullptr;
        blend_factor = 1.0f;
    }

    // Advance time and produce a local pose for this layer
    // Returns true if the layer produced a valid pose
    bool update(float dt, int bone_count, Pose& out_pose)
    {
        if (!playing || !clip)
        {
            return false;
        }

        // Advance time
        playback_time += dt * playback_speed;

        if (playback_time >= clip->duration)
        {
            if (looping)
            {
                playback_time = std::fmod(playback_time, clip->duration);
            }
            else
            {
                playback_time = clip->duration;
                playing = false;
            }
        }

        // Sample current clip
        clip->samplePose(playback_time, bone_count, out_pose);

        // Handle crossfade blending within this layer
        if (blend_from_clip && blend_factor < 1.0f)
        {
            blend_elapsed += dt;
            blend_factor = std::min(blend_elapsed / blend_duration, 1.0f);

            // Sample old clip
            Pose old_pose(bone_count);
            float old_time = blend_from_time + blend_elapsed * playback_speed;
            if (blend_from_clip->duration > 0.0f && old_time > blend_from_clip->duration)
            {
                old_time = std::fmod(old_time, blend_from_clip->duration);
            }
            blend_from_clip->samplePose(old_time, bone_count, old_pose);

            // Blend old -> new using quaternion slerp
            out_pose = Pose::blend(old_pose, out_pose, blend_factor);

            if (blend_factor >= 1.0f)
            {
                blend_from_clip = nullptr;
            }
        }

        return true;
    }
};
