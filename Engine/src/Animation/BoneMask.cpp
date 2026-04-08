#include "BoneMask.hpp"
#include "Skeleton.hpp"
#include <algorithm>

BoneMask::BoneMask(int bone_count, float default_weight)
    : weights(bone_count, default_weight)
{
}

void BoneMask::setWeight(int bone_index, float weight)
{
    if (bone_index >= 0 && bone_index < static_cast<int>(weights.size()))
    {
        weights[bone_index] = weight;
    }
}

float BoneMask::getWeight(int bone_index) const
{
    if (bone_index >= 0 && bone_index < static_cast<int>(weights.size()))
    {
        return weights[bone_index];
    }
    return 1.0f;
}

void BoneMask::setBoneAndChildren(const Skeleton& skeleton, int bone_index, float weight)
{
    const auto& bones = skeleton.getBones();
    int count = static_cast<int>(bones.size());

    if (bone_index < 0 || bone_index >= count) return;

    // Resize if needed
    if (static_cast<int>(weights.size()) < count)
    {
        weights.resize(count, 1.0f);
    }

    // Track which bones are in the affected set
    std::vector<bool> affected(count, false);
    affected[bone_index] = true;
    weights[bone_index] = weight;

    // Forward scan: bones are stored parent-before-child,
    // so any child's parent_id will already have been processed
    for (int i = bone_index + 1; i < count; i++)
    {
        if (bones[i].parent_id >= 0 && affected[bones[i].parent_id])
        {
            affected[i] = true;
            weights[i] = weight;
        }
    }
}

void BoneMask::setBoneAndChildren(const Skeleton& skeleton, const std::string& bone_name, float weight)
{
    int index = skeleton.getBoneIndex(bone_name);
    if (index >= 0)
    {
        setBoneAndChildren(skeleton, index, weight);
    }
}

BoneMask BoneMask::upperBody(const Skeleton& skeleton)
{
    int count = skeleton.getBoneCount();
    BoneMask mask(count, 0.0f); // start with nothing

    // Try common spine bone names
    static const char* spine_names[] = {
        "Spine", "spine", "Spine1", "spine1",
        "mixamorig:Spine", "mixamorig:Spine1",
        "Bip01_Spine", "Bip001_Spine"
    };

    for (const char* name : spine_names)
    {
        int idx = skeleton.getBoneIndex(name);
        if (idx >= 0)
        {
            mask.setBoneAndChildren(skeleton, idx, 1.0f);
            break;
        }
    }

    return mask;
}

BoneMask BoneMask::lowerBody(const Skeleton& skeleton)
{
    int count = skeleton.getBoneCount();
    BoneMask mask(count, 1.0f); // start with everything

    // Find spine and zero out upper body
    static const char* spine_names[] = {
        "Spine", "spine", "Spine1", "spine1",
        "mixamorig:Spine", "mixamorig:Spine1",
        "Bip01_Spine", "Bip001_Spine"
    };

    for (const char* name : spine_names)
    {
        int idx = skeleton.getBoneIndex(name);
        if (idx >= 0)
        {
            mask.setBoneAndChildren(skeleton, idx, 0.0f);
            break;
        }
    }

    return mask;
}
