#pragma once

#include "Animation/AnimationResourceService.hpp"

#include <memory>

class AnimationResourceSet;

// Runtime-only ECS state for an authored resource request. This is excluded
// from reflection and keeps cache ownership separate from the entity.
struct AnimResourceBindingComponent
{
    AnimationLoadTicket ticket;
    AnimationResourceKey requested_key;
    std::shared_ptr<const AnimationResourceSet> active;
    AnimationResourceState state = AnimationResourceState::Unrequested;
    bool diagnostic_emitted = false;
};
