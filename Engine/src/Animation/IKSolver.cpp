#include "IKSolver.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>

// Helper: extract world position from a 4x4 transform matrix
static glm::vec3 extractPosition(const glm::mat4& m)
{
    return glm::vec3(m[3]);
}

// Helper: extract rotation quaternion from a 4x4 transform matrix
static glm::quat extractRotation(const glm::mat4& m)
{
    glm::vec3 scale;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(m, scale, rotation, translation, skew, perspective);
    return rotation;
}

// Helper: recompute global transforms for a chain of bones after local pose changes
static void recomputeGlobals(const Skeleton& skeleton, const Pose& local_pose,
                              std::vector<glm::mat4>& global_transforms,
                              int start_bone)
{
    const auto& bones = skeleton.getBones();
    int count = static_cast<int>(bones.size());

    for (int i = start_bone; i < count; i++)
    {
        const glm::mat4 local = (i < local_pose.getBoneCount())
            ? local_pose[i].toMatrix() : bones[i].local_transform;

        if (bones[i].parent_id >= 0 && bones[i].parent_id < count)
        {
            global_transforms[i] = global_transforms[bones[i].parent_id] * local;
        }
        else
        {
            global_transforms[i] = local;
        }
    }
}

// ---- Two-Bone IK ----

void TwoBoneIKConstraint::solve(const Skeleton& skeleton,
                                 Pose& local_pose,
                                 std::vector<glm::mat4>& global_transforms,
                                 const TwoBoneIKConstraint& constraint)
{
    if (constraint.root_bone < 0 || constraint.mid_bone < 0 || constraint.end_bone < 0)
        return;

    if (constraint.weight <= 0.0f)
        return;

    int bone_count = static_cast<int>(global_transforms.size());
    if (constraint.root_bone >= bone_count || constraint.mid_bone >= bone_count || constraint.end_bone >= bone_count)
        return;

    // Get current world positions
    glm::vec3 root_pos = extractPosition(global_transforms[constraint.root_bone]);
    glm::vec3 mid_pos = extractPosition(global_transforms[constraint.mid_bone]);
    glm::vec3 end_pos = extractPosition(global_transforms[constraint.end_bone]);

    // Bone lengths
    float len_upper = glm::length(mid_pos - root_pos);
    float len_lower = glm::length(end_pos - mid_pos);

    // Avoid zero-length bones
    if (len_upper < 1e-6f || len_lower < 1e-6f)
        return;

    float max_reach = len_upper + len_lower;
    float min_reach = std::abs(len_upper - len_lower);

    // Direction and distance to target
    glm::vec3 to_target = constraint.target - root_pos;
    float dist_to_target = glm::length(to_target);

    // Clamp target distance to reachable range
    dist_to_target = glm::clamp(dist_to_target, min_reach + 1e-4f, max_reach - 1e-4f);

    // Direction to (clamped) target
    glm::vec3 dir_to_target = (glm::length(to_target) > 1e-6f)
        ? glm::normalize(to_target) : glm::vec3(0.0f, 1.0f, 0.0f);

    // --- Law of cosines to find mid joint angle ---
    // cos(angle_at_mid) = (L1^2 + L2^2 - D^2) / (2 * L1 * L2)
    float cos_mid = (len_upper * len_upper + len_lower * len_lower - dist_to_target * dist_to_target)
                    / (2.0f * len_upper * len_lower);
    cos_mid = glm::clamp(cos_mid, -1.0f, 1.0f);

    // cos(angle_at_root) = (L1^2 + D^2 - L2^2) / (2 * L1 * D)
    float cos_root = (len_upper * len_upper + dist_to_target * dist_to_target - len_lower * len_lower)
                     / (2.0f * len_upper * dist_to_target);
    cos_root = glm::clamp(cos_root, -1.0f, 1.0f);
    float angle_at_root = std::acos(cos_root);

    // --- Build orthonormal frame from target direction and pole vector ---
    glm::vec3 pole_dir = constraint.pole_vector - root_pos;
    // Make pole_dir perpendicular to dir_to_target
    pole_dir = pole_dir - glm::dot(pole_dir, dir_to_target) * dir_to_target;
    if (glm::length(pole_dir) < 1e-6f)
    {
        // Fallback: pick an arbitrary perpendicular
        pole_dir = glm::vec3(0.0f, 0.0f, 1.0f);
        pole_dir = pole_dir - glm::dot(pole_dir, dir_to_target) * dir_to_target;
        if (glm::length(pole_dir) < 1e-6f)
        {
            pole_dir = glm::vec3(0.0f, 1.0f, 0.0f);
            pole_dir = pole_dir - glm::dot(pole_dir, dir_to_target) * dir_to_target;
        }
    }
    pole_dir = glm::normalize(pole_dir);

    // Desired mid position
    glm::vec3 desired_mid = root_pos
        + dir_to_target * (len_upper * std::cos(angle_at_root))
        + pole_dir * (len_upper * std::sin(angle_at_root));

    // Desired end position (on the line from mid to target direction, at len_lower distance)
    glm::vec3 desired_end = root_pos + dir_to_target * dist_to_target;

    // --- Compute new local rotations ---

    // Save original rotations for weight blending
    glm::quat orig_root_rot = local_pose[constraint.root_bone].rotation;
    glm::quat orig_mid_rot = local_pose[constraint.mid_bone].rotation;

    // Get parent transforms
    const auto& bones = skeleton.getBones();

    // Root bone: compute rotation that moves the upper arm from its current direction to the desired direction
    glm::vec3 current_upper_dir = glm::normalize(mid_pos - root_pos);
    glm::vec3 desired_upper_dir = glm::normalize(desired_mid - root_pos);

    // Get inverse of parent's global rotation to work in parent space
    glm::quat root_parent_rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (bones[constraint.root_bone].parent_id >= 0)
    {
        root_parent_rot = extractRotation(global_transforms[bones[constraint.root_bone].parent_id]);
    }
    glm::quat root_global_rot = extractRotation(global_transforms[constraint.root_bone]);

    // Rotation from current to desired upper bone direction (in world space)
    glm::quat world_correction(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::length(glm::cross(current_upper_dir, desired_upper_dir)) > 1e-6f)
    {
        world_correction = glm::rotation(current_upper_dir, desired_upper_dir);
    }
    else if (glm::dot(current_upper_dir, desired_upper_dir) < -0.999f)
    {
        // 180 degree rotation: use pole_dir as rotation axis
        world_correction = glm::angleAxis(glm::pi<float>(), pole_dir);
    }

    // Convert to local space: new_local = inv(parent_global_rot) * world_correction * parent_global_rot * old_local
    glm::quat new_root_local = glm::inverse(root_parent_rot) * world_correction * root_parent_rot * orig_root_rot;
    new_root_local = glm::normalize(new_root_local);

    // Apply root rotation and recompute globals for mid bone
    local_pose[constraint.root_bone].rotation = new_root_local;
    recomputeGlobals(skeleton, local_pose, global_transforms, constraint.root_bone);

    // Mid bone: rotate so that lower arm points toward desired end
    glm::vec3 new_mid_pos = extractPosition(global_transforms[constraint.mid_bone]);
    glm::vec3 new_end_pos = extractPosition(global_transforms[constraint.end_bone]);

    glm::vec3 current_lower_dir = glm::normalize(new_end_pos - new_mid_pos);
    glm::vec3 desired_lower_dir = glm::normalize(desired_end - new_mid_pos);

    glm::quat mid_parent_rot = extractRotation(global_transforms[bones[constraint.mid_bone].parent_id]);

    glm::quat mid_world_correction(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::length(glm::cross(current_lower_dir, desired_lower_dir)) > 1e-6f)
    {
        mid_world_correction = glm::rotation(current_lower_dir, desired_lower_dir);
    }
    else if (glm::dot(current_lower_dir, desired_lower_dir) < -0.999f)
    {
        mid_world_correction = glm::angleAxis(glm::pi<float>(), pole_dir);
    }

    glm::quat new_mid_local = glm::inverse(mid_parent_rot) * mid_world_correction * mid_parent_rot * orig_mid_rot;
    new_mid_local = glm::normalize(new_mid_local);

    local_pose[constraint.mid_bone].rotation = new_mid_local;

    // Apply weight: slerp between original and solved
    if (constraint.weight < 1.0f)
    {
        glm::quat weighted_root = orig_root_rot;
        if (glm::dot(orig_root_rot, new_root_local) < 0.0f) new_root_local = -new_root_local;
        weighted_root = glm::slerp(orig_root_rot, new_root_local, constraint.weight);

        glm::quat weighted_mid = orig_mid_rot;
        if (glm::dot(orig_mid_rot, new_mid_local) < 0.0f) new_mid_local = -new_mid_local;
        weighted_mid = glm::slerp(orig_mid_rot, new_mid_local, constraint.weight);

        local_pose[constraint.root_bone].rotation = glm::normalize(weighted_root);
        local_pose[constraint.mid_bone].rotation = glm::normalize(weighted_mid);
    }
    else
    {
        local_pose[constraint.root_bone].rotation = new_root_local;
        local_pose[constraint.mid_bone].rotation = new_mid_local;
    }

    // Recompute global transforms for the affected chain
    recomputeGlobals(skeleton, local_pose, global_transforms,
                     std::min({constraint.root_bone, constraint.mid_bone, constraint.end_bone}));
}

// ---- FABRIK ----

void FABRIKConstraint::solve(const Skeleton& skeleton,
                              Pose& local_pose,
                              std::vector<glm::mat4>& global_transforms,
                              const FABRIKConstraint& constraint)
{
    int chain_len = static_cast<int>(constraint.chain.size());
    if (chain_len < 2) return;
    if (constraint.weight <= 0.0f) return;

    int bone_count = static_cast<int>(global_transforms.size());

    // Validate chain indices
    for (int idx : constraint.chain)
    {
        if (idx < 0 || idx >= bone_count) return;
    }

    // Extract current world positions for each joint in the chain
    std::vector<glm::vec3> positions(chain_len);
    for (int i = 0; i < chain_len; i++)
    {
        positions[i] = extractPosition(global_transforms[constraint.chain[i]]);
    }

    // Save original positions for weight blending
    std::vector<glm::vec3> original_positions = positions;

    // Compute bone lengths between consecutive joints
    std::vector<float> bone_lengths(chain_len - 1);
    float total_length = 0.0f;
    for (int i = 0; i < chain_len - 1; i++)
    {
        bone_lengths[i] = glm::length(positions[i + 1] - positions[i]);
        total_length += bone_lengths[i];
    }

    // Check if target is reachable
    float dist_to_target = glm::length(constraint.target - positions[0]);

    glm::vec3 target = constraint.target;

    // If target is beyond reach, extend chain toward target
    if (dist_to_target >= total_length)
    {
        glm::vec3 dir = glm::normalize(target - positions[0]);
        for (int i = 1; i < chain_len; i++)
        {
            positions[i] = positions[i - 1] + dir * bone_lengths[i - 1];
        }
    }
    else
    {
        // FABRIK iteration
        glm::vec3 root_pos = positions[0]; // root is fixed

        for (int iter = 0; iter < constraint.max_iterations; iter++)
        {
            // Check convergence
            float tip_dist = glm::length(positions[chain_len - 1] - target);
            if (tip_dist < constraint.tolerance) break;

            // Forward reaching: from tip to root
            positions[chain_len - 1] = target;
            for (int i = chain_len - 2; i >= 0; i--)
            {
                glm::vec3 dir = positions[i] - positions[i + 1];
                float len = glm::length(dir);
                if (len > 1e-6f)
                    dir /= len;
                else
                    dir = glm::vec3(0.0f, 1.0f, 0.0f);

                positions[i] = positions[i + 1] + dir * bone_lengths[i];
            }

            // Backward reaching: from root to tip
            positions[0] = root_pos;
            for (int i = 0; i < chain_len - 1; i++)
            {
                glm::vec3 dir = positions[i + 1] - positions[i];
                float len = glm::length(dir);
                if (len > 1e-6f)
                    dir /= len;
                else
                    dir = glm::vec3(0.0f, 1.0f, 0.0f);

                positions[i + 1] = positions[i] + dir * bone_lengths[i];
            }
        }
    }

    // Apply weight: blend between original and solved positions
    if (constraint.weight < 1.0f)
    {
        for (int i = 0; i < chain_len; i++)
        {
            positions[i] = glm::mix(original_positions[i], positions[i], constraint.weight);
        }
    }

    // Convert world positions back to local rotations
    const auto& bones = skeleton.getBones();

    // Save original local rotations
    std::vector<glm::quat> original_rotations(chain_len);
    for (int i = 0; i < chain_len; i++)
    {
        original_rotations[i] = local_pose[constraint.chain[i]].rotation;
    }

    for (int i = 0; i < chain_len - 1; i++)
    {
        int bone_idx = constraint.chain[i];
        int child_idx = constraint.chain[i + 1];

        // Current bone direction (from global transforms before IK)
        glm::vec3 current_dir = glm::normalize(
            extractPosition(global_transforms[child_idx]) - extractPosition(global_transforms[bone_idx]));

        // Desired bone direction (from solved positions)
        glm::vec3 desired_dir = glm::normalize(positions[i + 1] - positions[i]);

        // Skip if directions are too similar or bone length is zero
        if (glm::length(positions[i + 1] - positions[i]) < 1e-6f) continue;

        // Compute world-space rotation correction
        glm::quat correction(1.0f, 0.0f, 0.0f, 0.0f);
        float cross_len = glm::length(glm::cross(current_dir, desired_dir));
        if (cross_len > 1e-6f)
        {
            correction = glm::rotation(current_dir, desired_dir);
        }
        else if (glm::dot(current_dir, desired_dir) < -0.999f)
        {
            // 180 degrees apart - need arbitrary perpendicular axis
            glm::vec3 perp = glm::vec3(0.0f, 0.0f, 1.0f);
            if (std::abs(glm::dot(current_dir, perp)) > 0.9f)
                perp = glm::vec3(0.0f, 1.0f, 0.0f);
            perp = glm::normalize(glm::cross(current_dir, perp));
            correction = glm::angleAxis(glm::pi<float>(), perp);
        }

        // Convert to local space
        glm::quat parent_rot(1.0f, 0.0f, 0.0f, 0.0f);
        if (bones[bone_idx].parent_id >= 0)
        {
            parent_rot = extractRotation(global_transforms[bones[bone_idx].parent_id]);
        }

        glm::quat old_local = local_pose[bone_idx].rotation;
        glm::quat new_local = glm::inverse(parent_rot) * correction * parent_rot * old_local;
        new_local = glm::normalize(new_local);

        local_pose[bone_idx].rotation = new_local;

        // Recompute globals for subsequent bones in the chain
        recomputeGlobals(skeleton, local_pose, global_transforms, bone_idx);
    }
}
