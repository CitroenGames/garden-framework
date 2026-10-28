#pragma once

#include "Animation/AnimationResourceSet.hpp"
#include "Animation/CompiledAnimationFormat.hpp"
#include "Animation/AnimationResourceValidation.hpp"
#include "EngineExport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A skin table entry remains independently valid after decoding. The explicit
// vector of clips is retained for the cooker; runtime clients normally consume
// the immutable resource pointer.
struct CompiledAnimationSkin
{
    uint32_t skin_index = 0;
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<std::shared_ptr<const AnimationClip>> clips;
    std::string source_signature;
    std::shared_ptr<const AnimationResourceSet> resource;
};

struct CompiledAnimationData
{
    CompiledAnimationHeader header;
    std::vector<CompiledAnimationSkin> skins;
};

class ENGINE_API CompiledAnimationSerializer
{
public:
    // The serializer only publishes a completed data object on success. Errors
    // use stable CANM_* structural codes or the shared ANIM_* validators.
    static bool save(const CompiledAnimationData& data, const std::string& filepath,
                     AnimationResourceError* out_error = nullptr);
    static bool load(CompiledAnimationData& data, const std::string& filepath,
                     AnimationResourceError* out_error = nullptr);
};
