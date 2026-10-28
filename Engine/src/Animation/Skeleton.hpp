#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Pose;
class AnimationResourceBuilder;

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
    int getBoneCount() const { return static_cast<int>(m_bones.size()); }
    const Bone* getBone(int id) const;
    const Bone* getBoneByName(const std::string& name) const;
    int getBoneIndex(const std::string& name) const;

    void computeFinalMatrices(const std::vector<glm::mat4>& local_poses,
                              std::vector<glm::mat4>& out_final) const;
    void computeFinalMatrices(const Pose& local_pose,
                              std::vector<glm::mat4>& out_final) const;
    void computeGlobalTransforms(const Pose& local_pose,
                                 std::vector<glm::mat4>& out_global) const;

    const std::vector<Bone>& getBones() const { return m_bones; }

private:
    friend class AnimationResourceBuilder;

    explicit Skeleton(std::vector<Bone> bones);

    std::vector<Bone> m_bones;
    std::unordered_map<std::string, int> m_name_to_index;
};
