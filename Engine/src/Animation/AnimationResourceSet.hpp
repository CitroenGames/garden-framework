#pragma once

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class AnimationResourceBuilder;

class AnimationResourceSet;

class AnimationResourceSet
{
public:
    const Skeleton* skeleton() const { return m_skeleton.get(); }
    std::shared_ptr<const AnimationClip> findClip(const std::string& name) const
    {
        const auto it = m_clips.find(name);
        return it == m_clips.end() ? nullptr : it->second;
    }
    size_t clipCount() const { return m_clips.size(); }
    const std::string& sourceSignature() const { return m_source_signature; }

private:
    friend class AnimationResourceBuilder;


    AnimationResourceSet(std::shared_ptr<const Skeleton> skeleton,
                         std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> clips,
                         std::string source_signature)
        : m_skeleton(std::move(skeleton)), m_clips(std::move(clips)),
          m_source_signature(std::move(source_signature)) {}

    std::shared_ptr<const Skeleton> m_skeleton;
    std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> m_clips;
    std::string m_source_signature;
};
