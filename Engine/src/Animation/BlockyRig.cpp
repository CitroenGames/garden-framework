#include "Animation/BlockyRig.hpp"

#include "Animation/Skeleton.hpp"

#include <cmath>
#include <unordered_set>
#include <utility>

namespace
{
bool finite(float value)
{
    return std::isfinite(value);
}

bool finite(const glm::vec3& value)
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const glm::quat& value)
{
    return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
}

bool validPose(const BonePose& pose)
{
    constexpr float minimum_scale = 1e-8f;
    const float rotation_length = glm::length(pose.rotation);
    return finite(pose.translation) && finite(pose.scale) && finite(pose.rotation) &&
           finite(rotation_length) && rotation_length > minimum_scale &&
           std::abs(pose.scale.x) > minimum_scale &&
           std::abs(pose.scale.y) > minimum_scale &&
           std::abs(pose.scale.z) > minimum_scale;
}

void fail(BlockyRigError& error, const char* code, std::string message)
{
    error.code = code;
    error.message = std::move(message);
}
} // namespace

class BlockyRigBuilder final
{
public:
    static std::shared_ptr<const BlockyRig> publish(BlockyRigDefinition definition)
    {
        return std::shared_ptr<const BlockyRig>(new BlockyRig(
            std::move(definition.nodes), std::move(definition.source_signature)));
    }
};

namespace BlockyRigValidation
{
bool validate(const BlockyRigDefinition& definition, BlockyRigError& error)
{
    error = {};
    if (definition.format_version != BlockyRigDefinition::CurrentFormatVersion)
    {
        fail(error, "BLOCKY_RIG_VERSION", "blocky rig format version is not current");
        return false;
    }
    if (definition.source_signature.empty())
    {
        fail(error, "BLOCKY_RIG_SIGNATURE", "blocky rig source signature must be non-empty");
        return false;
    }
    if (definition.nodes.empty() || definition.nodes.size() > static_cast<size_t>(MAX_BONES))
    {
        fail(error, "BLOCKY_RIG_NODE_COUNT", "blocky rig node count must be in 1..MAX_BONES");
        return false;
    }

    std::unordered_set<std::string> names;
    for (size_t index = 0; index < definition.nodes.size(); ++index)
    {
        const auto& node = definition.nodes[index];
        if (node.name.empty() || !names.emplace(node.name).second)
        {
            fail(error, "BLOCKY_RIG_NODE_NAME", "blocky rig node names must be non-empty and unique");
            return false;
        }
        if (node.parent_index < -1 || node.parent_index >= static_cast<int>(index))
        {
            fail(error, "BLOCKY_RIG_PARENT_ORDER",
                 "blocky rig parents must precede children and roots use -1");
            return false;
        }
        if (!finite(node.pivot))
        {
            fail(error, "BLOCKY_RIG_PIVOT", "blocky rig pivots must be finite");
            return false;
        }
        if (!validPose(node.rest_pose))
        {
            fail(error, "BLOCKY_RIG_REST_POSE",
                 "blocky rig rest poses must be finite with non-zero rotation and scale");
            return false;
        }
    }
    return true;
}

std::shared_ptr<const BlockyRig> make(BlockyRigDefinition definition, BlockyRigError& error)
{
    if (!validate(definition, error))
        return nullptr;
    return BlockyRigBuilder::publish(std::move(definition));
}
} // namespace BlockyRigValidation
