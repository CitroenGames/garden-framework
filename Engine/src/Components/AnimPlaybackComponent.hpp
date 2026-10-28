#pragma once

#include "Animation/AnimationClip.hpp"

#include <memory>

// Transient per-entity clock, selected immutable clips, and blend state. It is
// seeded from SkeletonComponent defaults and intentionally has no
// reflection/serialization contract.
struct AnimPlaybackComponent
{
    std::shared_ptr<const AnimationClip> current_clip;
    std::shared_ptr<const AnimationClip> blend_from_clip;
    float time = 0.0f;
    float speed = 1.0f;
    float blend_from_time = 0.0f;
    float blend_duration = 0.0f;
    float blend_elapsed = 0.0f;
    bool looping = true;
    bool playing = false;
    bool initialized = false;
    bool completion_reported = false;
};