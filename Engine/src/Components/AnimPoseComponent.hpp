#pragma once

#include "Animation/Pose.hpp"

#include <glm/glm.hpp>

#include <vector>

// Transient per-entity presentation result. The palette is populated only by
// callers that request skinning output; dedicated-server scheduling can retain
// the evaluated local pose without allocating a palette.
struct AnimPoseComponent
{
    Pose local_pose;
    std::vector<glm::mat4> bone_matrices;
    bool pose_valid = false;
};