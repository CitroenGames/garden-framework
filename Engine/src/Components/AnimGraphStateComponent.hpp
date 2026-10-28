#pragma once

#include "Animation/AnimationGraph.hpp"

#include <cstdint>
#include <vector>

struct AnimGraphMachineState
{
    uint32_t active_state = UINT32_MAX;
    float state_time = 0.0f;
};

// Per-entity, inspectable runtime state. It intentionally holds no resource
// pointers and is not reflected/serialized in this foundation.
struct AnimGraphStateComponent
{
    std::vector<AnimGraphParameterValue> parameters;
    std::vector<AnimGraphMachineState> machines;
    const AnimGraph* initialized_graph = nullptr;
    bool initialized = false;
};
