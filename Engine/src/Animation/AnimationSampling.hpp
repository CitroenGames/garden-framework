#pragma once

#include "Animation/Skeleton.hpp"
#include "EngineExport.h"

class AnimationClip;
class Pose;

namespace AnimationSampling
{
// Decompose every immutable skeleton rest local into an ECS pose.
ENGINE_API void makeRestPose(const Skeleton& skeleton, Pose& out_pose);

// Apply only properties that have authored keys. Missing properties and bones
// preserve the rest values supplied by makeRestPose.
ENGINE_API void overlayClip(const AnimationClip& clip, float time, Pose& in_out_pose);
}
