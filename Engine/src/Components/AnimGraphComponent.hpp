#pragma once

#include "Animation/AnimationGraph.hpp"

#include <memory>

// Runtime binding to a shared immutable graph. A later authoring component
// supplies the serialized graph key; resource ownership stays with the graph
// service rather than with an entity.
struct AnimGraphComponent
{
    std::shared_ptr<const AnimGraph> graph;
};
