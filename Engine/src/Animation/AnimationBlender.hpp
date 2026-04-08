#pragma once

#include "AnimationClip.hpp"
#include "AnimationLayer.hpp"
#include "Pose.hpp"
#include <memory>
#include <vector>

class AnimationBlender
{
public:
    // --- Legacy API (operates on layer 0) ---

    // Play a clip (optionally with crossfade from current)
    void play(std::shared_ptr<AnimationClip> clip, float blend_time = 0.0f);

    // Stop playback
    void stop();

    // Set playback speed (1.0 = normal)
    void setSpeed(float speed);
    float getSpeed() const;

    // Set looping
    void setLooping(bool loop);
    bool isLooping() const;

    // Query
    bool isPlaying() const;
    bool isBlending() const;
    float getPlaybackTime() const;
    const AnimationClip* getCurrentClip() const;

    // Advance time and produce local pose matrices (legacy path)
    void update(float dt, int bone_count, std::vector<glm::mat4>& out_local_poses);

    // --- New Pose-based API ---

    // Advance time and produce a decomposed Pose (proper quaternion blending)
    void updatePose(float dt, int bone_count, Pose& out_pose);

    // --- Layer management ---

    int addLayer(BlendMode mode = BlendMode::Override);
    AnimationLayer& getLayer(int index);
    const AnimationLayer& getLayer(int index) const;
    void removeLayer(int index);
    int getLayerCount() const { return static_cast<int>(layers.size()); }

private:
    std::vector<AnimationLayer> layers;

    // Ensure layer 0 exists
    void ensureBaseLayer();
};
