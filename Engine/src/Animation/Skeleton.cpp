#include "Skeleton.hpp"
#include "Pose.hpp"

#include <utility>

Skeleton::Skeleton(std::vector<Bone> bones)
    : m_bones(std::move(bones))
{
    for (const Bone& bone : m_bones)
        m_name_to_index.emplace(bone.name, bone.id);
}

const Bone* Skeleton::getBone(int id) const
{
    if (id < 0 || id >= static_cast<int>(m_bones.size())) return nullptr;
    return &m_bones[id];
}

const Bone* Skeleton::getBoneByName(const std::string& name) const
{
    auto it = m_name_to_index.find(name);
    return it == m_name_to_index.end() ? nullptr : &m_bones[it->second];
}

int Skeleton::getBoneIndex(const std::string& name) const
{
    auto it = m_name_to_index.find(name);
    return it != m_name_to_index.end() ? it->second : -1;
}

void Skeleton::computeFinalMatrices(const std::vector<glm::mat4>& local_poses,
                                     std::vector<glm::mat4>& out_final) const
{
    const int count = static_cast<int>(m_bones.size());
    out_final.resize(count, glm::mat4(1.0f));
    std::vector<glm::mat4> global_transforms(count, glm::mat4(1.0f));
    for (int i = 0; i < count; i++)
    {
        const glm::mat4& local = i < static_cast<int>(local_poses.size()) ? local_poses[i] : m_bones[i].local_transform;
        global_transforms[i] = m_bones[i].parent_id >= 0 ? global_transforms[m_bones[i].parent_id] * local : local;
        out_final[i] = global_transforms[i] * m_bones[i].inverse_bind_matrix;
    }
}

void Skeleton::computeFinalMatrices(const Pose& local_pose,
                                     std::vector<glm::mat4>& out_final) const
{
    const int count = static_cast<int>(m_bones.size());
    out_final.resize(count, glm::mat4(1.0f));
    std::vector<glm::mat4> global_transforms(count, glm::mat4(1.0f));
    for (int i = 0; i < count; i++)
    {
        const glm::mat4 local = i < local_pose.getBoneCount() ? local_pose[i].toMatrix() : m_bones[i].local_transform;
        global_transforms[i] = m_bones[i].parent_id >= 0 ? global_transforms[m_bones[i].parent_id] * local : local;
        out_final[i] = global_transforms[i] * m_bones[i].inverse_bind_matrix;
    }
}

void Skeleton::computeGlobalTransforms(const Pose& local_pose,
                                        std::vector<glm::mat4>& out_global) const
{
    const int count = static_cast<int>(m_bones.size());
    out_global.resize(count, glm::mat4(1.0f));
    for (int i = 0; i < count; i++)
    {
        const glm::mat4 local = i < local_pose.getBoneCount() ? local_pose[i].toMatrix() : m_bones[i].local_transform;
        out_global[i] = m_bones[i].parent_id >= 0 ? out_global[m_bones[i].parent_id] * local : local;
    }
}
