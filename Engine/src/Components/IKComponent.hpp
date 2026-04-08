#pragma once

#include "Animation/IKSolver.hpp"
#include <vector>
#include <string>

struct IKComponent
{
    std::vector<TwoBoneIKConstraint> two_bone_constraints;
    std::vector<FABRIKConstraint> fabrik_constraints;
    bool enabled = true;

    // Convenience: set up foot IK (thigh -> shin -> foot)
    void addFootIK(const Skeleton& skeleton,
                   const std::string& thigh_name,
                   const std::string& shin_name,
                   const std::string& foot_name)
    {
        TwoBoneIKConstraint c;
        c.root_bone = skeleton.getBoneIndex(thigh_name);
        c.mid_bone = skeleton.getBoneIndex(shin_name);
        c.end_bone = skeleton.getBoneIndex(foot_name);

        if (c.root_bone >= 0 && c.mid_bone >= 0 && c.end_bone >= 0)
        {
            two_bone_constraints.push_back(c);
        }
    }

    // Convenience: set up hand IK (upper arm -> forearm -> hand)
    void addHandIK(const Skeleton& skeleton,
                   const std::string& upper_arm_name,
                   const std::string& forearm_name,
                   const std::string& hand_name)
    {
        TwoBoneIKConstraint c;
        c.root_bone = skeleton.getBoneIndex(upper_arm_name);
        c.mid_bone = skeleton.getBoneIndex(forearm_name);
        c.end_bone = skeleton.getBoneIndex(hand_name);

        if (c.root_bone >= 0 && c.mid_bone >= 0 && c.end_bone >= 0)
        {
            two_bone_constraints.push_back(c);
        }
    }

    // Add a FABRIK chain by bone names (from root to tip)
    void addFABRIKChain(const Skeleton& skeleton,
                        const std::vector<std::string>& bone_names)
    {
        FABRIKConstraint c;
        for (const auto& name : bone_names)
        {
            int idx = skeleton.getBoneIndex(name);
            if (idx < 0) return; // invalid chain
            c.chain.push_back(idx);
        }

        if (c.chain.size() >= 2)
        {
            fabrik_constraints.push_back(c);
        }
    }
};
