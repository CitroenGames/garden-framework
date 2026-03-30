#include "AnimationBlender.hpp"
#include <algorithm>

void AnimationBlender::play(std::shared_ptr<AnimationClip> clip, float blend_time)
{
    if (!clip) return;

    if (blend_time > 0.0f && current_clip && playing)
    {
        // Crossfade: old clip becomes blend_clip
        blend_clip = current_clip;
        blend_from_time = playback_time;
        blend_duration = blend_time;
        blend_elapsed = 0.0f;
        blend_factor = 0.0f;
    }
    else
    {
        blend_clip = nullptr;
        blend_factor = 1.0f;
    }

    current_clip = std::move(clip);
    playback_time = 0.0f;
    playing = true;
}

void AnimationBlender::stop()
{
    playing = false;
    blend_clip = nullptr;
    blend_factor = 1.0f;
}

void AnimationBlender::update(float dt, int bone_count, std::vector<glm::mat4>& out_local_poses)
{
    if (!playing || !current_clip)
    {
        out_local_poses.resize(bone_count, glm::mat4(1.0f));
        return;
    }

    // Advance time
    playback_time += dt * playback_speed;

    if (playback_time >= current_clip->duration)
    {
        if (looping)
        {
            playback_time = std::fmod(playback_time, current_clip->duration);
        }
        else
        {
            playback_time = current_clip->duration;
            playing = false;
        }
    }

    // Sample current clip
    current_clip->sample(playback_time, bone_count, out_local_poses);

    // Handle crossfade blending
    if (blend_clip && blend_factor < 1.0f)
    {
        blend_elapsed += dt;
        blend_factor = std::min(blend_elapsed / blend_duration, 1.0f);

        // Sample old clip
        std::vector<glm::mat4> old_poses;
        float old_time = blend_from_time + blend_elapsed * playback_speed;
        if (old_time > blend_clip->duration)
        {
            old_time = std::fmod(old_time, blend_clip->duration);
        }
        blend_clip->sample(old_time, bone_count, old_poses);

        // Lerp between old and new poses per-bone
        // Simple matrix lerp (not ideal for large rotations, but good enough for short blends)
        for (int i = 0; i < bone_count && i < static_cast<int>(old_poses.size()); i++)
        {
            // Component-wise lerp of matrices
            for (int col = 0; col < 4; col++)
            {
                out_local_poses[i][col] = glm::mix(old_poses[i][col], out_local_poses[i][col], blend_factor);
            }
        }

        if (blend_factor >= 1.0f)
        {
            blend_clip = nullptr;
        }
    }
}
