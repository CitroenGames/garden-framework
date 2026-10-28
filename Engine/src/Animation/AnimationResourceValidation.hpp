#pragma once

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"
#include "EngineExport.h"

#include <memory>
#include <string>
#include <vector>

class AnimationResourceSet;

struct AnimationResourceError
{
    std::string code;
    std::string message;
};

// Raw decoder input. These types are deliberately separate from the immutable
// runtime objects so a source or package decoder cannot publish partial data.
struct AnimationClipDefinition
{
    std::string name;
    float duration = 0.0f;
    float ticks_per_second = 25.0f;
    std::vector<BoneAnimation> channels;
};

namespace AnimationResourceValidation
{
ENGINE_API bool validateSkeleton(const std::vector<Bone>& bones, AnimationResourceError& error);
ENGINE_API bool validateClip(AnimationClipDefinition& clip, int bone_count,
                             AnimationResourceError& error);
ENGINE_API std::shared_ptr<const Skeleton> makeSkeleton(std::vector<Bone> bones,
                                                         AnimationResourceError& error);
ENGINE_API std::shared_ptr<const AnimationClip> makeClip(AnimationClipDefinition clip,
                                                          int bone_count,
                                                          AnimationResourceError& error);
ENGINE_API std::shared_ptr<const AnimationResourceSet> makeResourceSet(
    std::shared_ptr<const Skeleton> skeleton,
    std::vector<std::shared_ptr<const AnimationClip>> clips,
    std::string source_signature,
    AnimationResourceError& error);
} // namespace AnimationResourceValidation
