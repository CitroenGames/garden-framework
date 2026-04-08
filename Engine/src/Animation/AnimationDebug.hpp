#pragma once

#include "Pose.hpp"
#include "Skeleton.hpp"
#include "Components/IKComponent.hpp"
#include "Debug/DebugDraw.hpp"
#include <glm/glm.hpp>

namespace AnimationDebug
{

// Draw skeleton hierarchy as lines connecting joints
inline void drawSkeleton(const Skeleton& skeleton, const Pose& pose,
                          const glm::mat4& world_transform,
                          const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f))
{
    const auto& bones = skeleton.getBones();
    int count = skeleton.getBoneCount();

    // Compute model-space transforms
    std::vector<glm::mat4> globals;
    skeleton.computeGlobalTransforms(pose, globals);

    for (int i = 0; i < count; i++)
    {
        glm::vec3 bone_pos = glm::vec3(world_transform * globals[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

        // Draw joint point
        DebugDraw::get().drawPoint(bone_pos, 0.02f, color);

        // Draw line to parent
        if (bones[i].parent_id >= 0 && bones[i].parent_id < count)
        {
            glm::vec3 parent_pos = glm::vec3(world_transform * globals[bones[i].parent_id] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            DebugDraw::get().drawLine(parent_pos, bone_pos, color);
        }
    }
}

// Draw IK targets as spheres and pole vectors as lines
inline void drawIKTargets(const IKComponent& ik,
                           const glm::vec3& target_color = glm::vec3(1.0f, 0.0f, 0.0f),
                           const glm::vec3& pole_color = glm::vec3(0.0f, 0.0f, 1.0f))
{
    for (const auto& constraint : ik.two_bone_constraints)
    {
        // Draw target
        DebugDraw::get().drawSphere(constraint.target, 0.05f, target_color);

        // Draw pole vector hint
        DebugDraw::get().drawSphere(constraint.pole_vector, 0.03f, pole_color);
        DebugDraw::get().drawLine(constraint.target, constraint.pole_vector, pole_color);
    }

    for (const auto& constraint : ik.fabrik_constraints)
    {
        DebugDraw::get().drawSphere(constraint.target, 0.05f, target_color);
    }
}

} // namespace AnimationDebug
