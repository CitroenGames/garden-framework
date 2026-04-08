#pragma once

#include "Pose.hpp"
#include "Skeleton.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <string>

// Two-Bone IK: analytical solution for 3-joint chains (e.g., shoulder-elbow-wrist, hip-knee-ankle)
// Uses the law of cosines to compute exact joint angles.
struct TwoBoneIKConstraint
{
    int root_bone = -1;  // e.g., upper arm / thigh
    int mid_bone = -1;   // e.g., elbow / knee
    int end_bone = -1;   // e.g., wrist / ankle

    glm::vec3 target{0.0f};       // world-space target position for end effector
    glm::vec3 pole_vector{0.0f, 0.0f, 1.0f}; // hint direction for the bend plane
    float weight = 1.0f;          // 0 = no IK, 1 = full IK

    // Solve in-place, modifying the local pose and global transforms
    static void solve(const Skeleton& skeleton,
                      Pose& local_pose,
                      std::vector<glm::mat4>& global_transforms,
                      const TwoBoneIKConstraint& constraint);
};

// FABRIK: Forward And Backward Reaching Inverse Kinematics
// Iterative solver for arbitrary-length bone chains.
struct FABRIKConstraint
{
    std::vector<int> chain; // bone indices from root to tip
    glm::vec3 target{0.0f};
    float weight = 1.0f;
    int max_iterations = 10;
    float tolerance = 0.001f; // stop when tip is within this distance of target

    // Solve in-place
    static void solve(const Skeleton& skeleton,
                      Pose& local_pose,
                      std::vector<glm::mat4>& global_transforms,
                      const FABRIKConstraint& constraint);
};
