#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>

class Pose;

static constexpr int MAX_BONES = 128;

struct Bone
{
    int id = -1;
    int parent_id = -1;
    std::string name;
    glm::mat4 inverse_bind_matrix{1.0f};
    glm::mat4 local_transform{1.0f};
};

class Skeleton
{
public:
    Skeleton() = default;

    void addBone(const Bone& bone);
    int getBoneCount() const { return static_cast<int>(bones.size()); }
    const Bone* getBone(int id) const;
    const Bone* getBoneByName(const std::string& name) const;
    int getBoneIndex(const std::string& name) const;

    // Given per-bone local transforms, compute final matrices (local * inverse_bind)
    // walking the hierarchy from root to leaf
    void computeFinalMatrices(const std::vector<glm::mat4>& local_poses,
                              std::vector<glm::mat4>& out_final) const;

    // Pose-based overload: same hierarchy walk but takes decomposed Pose
    void computeFinalMatrices(const Pose& local_pose,
                              std::vector<glm::mat4>& out_final) const;

    // Compute model-space global transforms WITHOUT inverse bind matrix
    // (needed by IK solvers which operate in model space)
    void computeGlobalTransforms(const Pose& local_pose,
                                  std::vector<glm::mat4>& out_global) const;

    const std::vector<Bone>& getBones() const { return bones; }

private:
    std::vector<Bone> bones;
    std::unordered_map<std::string, int> name_to_index;
};
