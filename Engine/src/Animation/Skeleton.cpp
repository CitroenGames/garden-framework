#include "Skeleton.hpp"

void Skeleton::addBone(const Bone& bone)
{
    int index = static_cast<int>(bones.size());
    bones.push_back(bone);
    bones.back().id = index;
    if (!bone.name.empty())
    {
        name_to_index[bone.name] = index;
    }
}

const Bone* Skeleton::getBone(int id) const
{
    if (id < 0 || id >= static_cast<int>(bones.size())) return nullptr;
    return &bones[id];
}

const Bone* Skeleton::getBoneByName(const std::string& name) const
{
    auto it = name_to_index.find(name);
    if (it == name_to_index.end()) return nullptr;
    return &bones[it->second];
}

int Skeleton::getBoneIndex(const std::string& name) const
{
    auto it = name_to_index.find(name);
    return it != name_to_index.end() ? it->second : -1;
}

void Skeleton::computeFinalMatrices(const std::vector<glm::mat4>& local_poses,
                                     std::vector<glm::mat4>& out_final) const
{
    int count = static_cast<int>(bones.size());
    out_final.resize(count, glm::mat4(1.0f));

    // Compute global transforms by walking parent chain
    // Bones are stored in hierarchy order (parent before child)
    std::vector<glm::mat4> global_transforms(count, glm::mat4(1.0f));

    for (int i = 0; i < count; i++)
    {
        const glm::mat4& local = (i < static_cast<int>(local_poses.size()))
            ? local_poses[i] : bones[i].local_transform;

        if (bones[i].parent_id >= 0 && bones[i].parent_id < count)
        {
            global_transforms[i] = global_transforms[bones[i].parent_id] * local;
        }
        else
        {
            global_transforms[i] = local;
        }

        // Final matrix = global_transform * inverse_bind_matrix
        out_final[i] = global_transforms[i] * bones[i].inverse_bind_matrix;
    }
}
