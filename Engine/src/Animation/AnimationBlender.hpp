#pragma once

#include "AnimationClip.hpp"
#include <memory>

class AnimationBlender
{
public:
    // Play a clip (optionally with crossfade from current)
    void play(std::shared_ptr<AnimationClip> clip, float blend_time = 0.0f);

    // Stop playback
    void stop();

    // Set playback speed (1.0 = normal)
    void setSpeed(float speed) { playback_speed = speed; }
    float getSpeed() const { return playback_speed; }

    // Set looping
    void setLooping(bool loop) { looping = loop; }
    bool isLooping() const { return looping; }

    // Query
    bool isPlaying() const { return playing; }
    bool isBlending() const { return blend_clip != nullptr && blend_factor < 1.0f; }
    float getPlaybackTime() const { return playback_time; }
    const AnimationClip* getCurrentClip() const { return current_clip.get(); }

    // Advance time and produce local pose matrices
    void update(float dt, int bone_count, std::vector<glm::mat4>& out_local_poses);

private:
    std::shared_ptr<AnimationClip> current_clip;
    std::shared_ptr<AnimationClip> blend_clip; // clip we're blending FROM

    float playback_time = 0.0f;
    float playback_speed = 1.0f;
    bool playing = false;
    bool looping = true;

    // Crossfade state
    float blend_factor = 1.0f;   // 1.0 = fully on current_clip
    float blend_duration = 0.0f;
    float blend_elapsed = 0.0f;
    float blend_from_time = 0.0f; // time in the old clip when blend started
};
