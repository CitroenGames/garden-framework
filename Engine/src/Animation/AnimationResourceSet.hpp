#pragma once

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

class AnimationResourceSet
{
public:
    AnimationResourceSet(std::shared_ptr<const Skeleton> skeleton,
                         std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> clips)
        : m_skeleton(std::move(skeleton)), m_clips(std::move(clips)) {}

    const Skeleton* skeleton() const { return m_skeleton.get(); }
    std::shared_ptr<const AnimationClip> findClip(const std::string& name) const
    {
        const auto it = m_clips.find(name);
        return it == m_clips.end() ? nullptr : it->second;
    }

private:
    std::shared_ptr<const Skeleton> m_skeleton;
    std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> m_clips;
};