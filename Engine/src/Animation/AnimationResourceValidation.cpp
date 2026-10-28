#include "AnimationResourceValidation.hpp"

#include "Animation/AnimationResourceSet.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
bool finite(float value) { return std::isfinite(value); }

bool finite(const glm::vec3& value)
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const glm::quat& value)
{
    return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const glm::mat4& value)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!finite(value[column][row])) return false;
    return true;
}

bool affine(const glm::mat4& value)
{
    constexpr float epsilon = 0.0001f;
    return std::abs(value[0][3]) <= epsilon && std::abs(value[1][3]) <= epsilon &&
           std::abs(value[2][3]) <= epsilon && std::abs(value[3][3] - 1.0f) <= epsilon;
}

bool validRestTransform(const glm::mat4& value)
{
    if (!finite(value) || !affine(value)) return false;
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (!glm::decompose(value, scale, rotation, translation, skew, perspective)) return false;
    const float rotation_length = glm::length(rotation);
    return finite(scale) && finite(translation) && finite(skew) && finite(perspective.x) &&
           finite(perspective.y) && finite(perspective.z) && finite(perspective.w) &&
           finite(rotation) && finite(rotation_length) && rotation_length > 0.0f;
}

template<typename Key>
bool validKeyTimes(const std::vector<Key>& keys)
{
    float previous = -1.0f;
    for (const Key& key : keys)
    {
        if (!finite(key.time) || key.time < 0.0f || (previous >= 0.0f && key.time <= previous))
            return false;
        previous = key.time;
    }
    return true;
}

bool validKeys(const BoneAnimation& channel)
{
    if (!validKeyTimes(channel.position_keys) || !validKeyTimes(channel.rotation_keys) ||
        !validKeyTimes(channel.scale_keys))
        return false;
    for (const PositionKey& key : channel.position_keys) if (!finite(key.value)) return false;
    for (const ScaleKey& key : channel.scale_keys) if (!finite(key.value)) return false;
    for (const RotationKey& key : channel.rotation_keys)
    {
        const float length = glm::length(key.value);
        if (!finite(key.value) || !finite(length) || length <= 0.0f) return false;
    }
    return true;
}

void fail(AnimationResourceError& error, const char* code, std::string message)
{
    error.code = code;
    error.message = std::move(message);
}
} // namespace

class AnimationResourceBuilder
{
public:
    static std::shared_ptr<const Skeleton> makeSkeleton(std::vector<Bone> bones)
    {
        return std::shared_ptr<const Skeleton>(new Skeleton(std::move(bones)));
    }

    static std::shared_ptr<const AnimationClip> makeClip(AnimationClipDefinition clip)
    {
        return std::shared_ptr<const AnimationClip>(new AnimationClip(
            std::move(clip.name), clip.duration, clip.ticks_per_second, std::move(clip.channels)));
    }

    static std::shared_ptr<const AnimationResourceSet> makeResourceSet(
        std::shared_ptr<const Skeleton> skeleton,
        std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> clips,
        std::string source_signature)
    {
        return std::shared_ptr<const AnimationResourceSet>(new AnimationResourceSet(
            std::move(skeleton), std::move(clips), std::move(source_signature)));
    }
};
namespace AnimationResourceValidation
{
bool validateSkeleton(const std::vector<Bone>& bones, AnimationResourceError& error)
{
    error = {};
    if (bones.empty() || bones.size() > static_cast<size_t>(MAX_BONES))
    {
        fail(error, "ANIM_BONE_COUNT", "skeleton bone count must be in 1..MAX_BONES");
        return false;
    }

    std::unordered_set<std::string> names;
    for (size_t index = 0; index < bones.size(); ++index)
    {
        const Bone& bone = bones[index];
        if (bone.id != static_cast<int>(index) || bone.parent_id >= static_cast<int>(index))
        {
            fail(error, "ANIM_BONE_PARENT_ORDER", "bone IDs must be ordered and parents must precede children");
            return false;
        }
        if (bone.parent_id < -1 || bone.parent_id >= static_cast<int>(bones.size()) ||
            bone.parent_id == static_cast<int>(index))
        {
            fail(error, "ANIM_BONE_HIERARCHY", "bone parent is out of range or references itself");
            return false;
        }
        if (bone.name.empty() || !names.emplace(bone.name).second)
        {
            fail(error, "ANIM_BONE_NAME", "bone names must be non-empty and unique");
            return false;
        }
        if (!finite(bone.inverse_bind_matrix))
        {
            fail(error, "ANIM_BONE_NONFINITE_MATRIX", "inverse bind matrix contains non-finite values");
            return false;
        }
        if (!validRestTransform(bone.local_transform))
        {
            fail(error, "ANIM_BONE_INVALID_REST", "rest local transform must be finite affine and decomposable");
            return false;
        }
    }
    return true;
}

bool validateClip(AnimationClipDefinition& clip, int bone_count, AnimationResourceError& error)
{
    error = {};
    if (clip.name.empty())
    {
        fail(error, "ANIM_CLIP_NAME", "clip name must be non-empty");
        return false;
    }
    if (!finite(clip.duration) || clip.duration < 0.0f || !finite(clip.ticks_per_second) ||
        clip.ticks_per_second <= 0.0f)
    {
        fail(error, "ANIM_CLIP_TIMING", "clip duration and tick rate must be finite");
        return false;
    }
    if (clip.channels.empty())
    {
        fail(error, "ANIM_EMPTY_CLIP", "clip must contain at least one channel");
        return false;
    }

    std::sort(clip.channels.begin(), clip.channels.end(), [](const BoneAnimation& a, const BoneAnimation& b) {
        return a.bone_index < b.bone_index;
    });
    int previous_bone = -1;
    float latest_key = 0.0f;
    for (BoneAnimation& channel : clip.channels)
    {
        if (channel.bone_index < 0 || channel.bone_index >= bone_count)
        {
            fail(error, "ANIM_CHANNEL_BONE", "channel targets a bone outside the skeleton");
            return false;
        }
        if (channel.bone_index == previous_bone)
        {
            fail(error, "ANIM_DUPLICATE_CHANNEL", "clip contains duplicate channels for one bone");
            return false;
        }
        previous_bone = channel.bone_index;
        if (channel.position_keys.empty() && channel.rotation_keys.empty() && channel.scale_keys.empty())
        {
            fail(error, "ANIM_EMPTY_CHANNEL", "channel must key translation, rotation, or scale");
            return false;
        }
        if (!validKeys(channel))
        {
            fail(error, "ANIM_INVALID_KEY", "key times and values must be finite and strictly increasing");
            return false;
        }
        for (RotationKey& key : channel.rotation_keys)
            key.value = glm::normalize(key.value);
        if (!channel.position_keys.empty()) latest_key = std::max(latest_key, channel.position_keys.back().time);
        if (!channel.rotation_keys.empty()) latest_key = std::max(latest_key, channel.rotation_keys.back().time);
        if (!channel.scale_keys.empty()) latest_key = std::max(latest_key, channel.scale_keys.back().time);
    }
    if (clip.duration < latest_key)
    {
        fail(error, "ANIM_CLIP_DURATION", "clip duration precedes its final key");
        return false;
    }
    return true;
}

std::shared_ptr<const Skeleton> makeSkeleton(std::vector<Bone> bones, AnimationResourceError& error)
{
    if (!validateSkeleton(bones, error)) return nullptr;
    return AnimationResourceBuilder::makeSkeleton(std::move(bones));
}

std::shared_ptr<const AnimationClip> makeClip(AnimationClipDefinition clip, int bone_count,
                                              AnimationResourceError& error)
{
    if (!validateClip(clip, bone_count, error)) return nullptr;
    return AnimationResourceBuilder::makeClip(std::move(clip));
}

std::shared_ptr<const AnimationResourceSet> makeResourceSet(
    std::shared_ptr<const Skeleton> skeleton,
    std::vector<std::shared_ptr<const AnimationClip>> clips,
    std::string source_signature,
    AnimationResourceError& error)
{
    error = {};
    if (!skeleton)
    {
        fail(error, "ANIM_RESOURCE_SKELETON", "resource set requires a validated skeleton");
        return nullptr;
    }
    if (source_signature.empty())
    {
        fail(error, "ANIM_RESOURCE_SIGNATURE", "resource set requires a source signature");
        return nullptr;
    }
    std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> by_name;
    for (const auto& clip : clips)
    {
        if (!clip)
        {
            fail(error, "ANIM_RESOURCE_CLIP", "resource set cannot contain null clips");
            return nullptr;
        }
        if (!by_name.emplace(clip->name(), clip).second)
        {
            fail(error, "ANIM_CLIP_NAME", "clip names must be unique in one resource set");
            return nullptr;
        }
    }
    return AnimationResourceBuilder::makeResourceSet(
        std::move(skeleton), std::move(by_name), std::move(source_signature));
}} // namespace AnimationResourceValidation


