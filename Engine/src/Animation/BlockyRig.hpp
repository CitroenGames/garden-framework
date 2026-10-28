#pragma once

#include "Animation/Pose.hpp"
#include "EngineExport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Current, decoded representation of a rigid-limb hierarchy. This is not a
// mesh asset: mesh binding remains ordinary entity authoring, while the rig
// only owns named local-pose nodes and their parent-before-child topology.
struct BlockyRigNodeDefinition
{
    std::string name;
    int parent_index = -1;
    glm::vec3 pivot{0.0f};
    BonePose rest_pose{};
    std::string mesh_slot;
};

struct BlockyRigDefinition
{
    static constexpr uint32_t CurrentFormatVersion = 1;

    uint32_t format_version = CurrentFormatVersion;
    std::string source_signature;
    std::vector<BlockyRigNodeDefinition> nodes;
};

struct BlockyRigError
{
    std::string code;
    std::string message;
};

class BlockyRig
{
public:
    static constexpr uint32_t CurrentFormatVersion = BlockyRigDefinition::CurrentFormatVersion;

    const std::vector<BlockyRigNodeDefinition>& nodes() const { return m_nodes; }
    const std::string& sourceSignature() const { return m_source_signature; }
    uint32_t formatVersion() const { return CurrentFormatVersion; }

private:
    friend class BlockyRigBuilder;

    BlockyRig(std::vector<BlockyRigNodeDefinition> nodes, std::string source_signature)
        : m_nodes(std::move(nodes)), m_source_signature(std::move(source_signature)) {}

    std::vector<BlockyRigNodeDefinition> m_nodes;
    std::string m_source_signature;
};

namespace BlockyRigValidation
{
// Rejects every non-current representation. There is intentionally no
// legacy reader or conversion path for rig definitions.
ENGINE_API bool validate(const BlockyRigDefinition& definition, BlockyRigError& error);
ENGINE_API std::shared_ptr<const BlockyRig> make(BlockyRigDefinition definition,
                                                 BlockyRigError& error);
} // namespace BlockyRigValidation
