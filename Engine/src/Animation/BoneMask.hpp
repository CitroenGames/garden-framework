#pragma once

#include <vector>
#include <string>

class Skeleton;

class BoneMask
{
public:
    BoneMask() = default;
    explicit BoneMask(int bone_count, float default_weight = 1.0f);

    void setWeight(int bone_index, float weight);
    float getWeight(int bone_index) const;

    // Set a bone and all its descendants to a given weight
    void setBoneAndChildren(const Skeleton& skeleton, int bone_index, float weight);
    void setBoneAndChildren(const Skeleton& skeleton, const std::string& bone_name, float weight);

    const std::vector<float>& getWeights() const { return weights; }
    int size() const { return static_cast<int>(weights.size()); }

    // Factory methods for common masks
    static BoneMask upperBody(const Skeleton& skeleton);
    static BoneMask lowerBody(const Skeleton& skeleton);

private:
    std::vector<float> weights;
};
