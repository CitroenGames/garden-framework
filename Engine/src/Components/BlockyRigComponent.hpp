#pragma once

#include "Animation/BlockyRig.hpp"
#include "Components/IdentityComponent.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// Immutable rig binding on a rig-root entity. The resource is deliberately a
// runtime handle here: authored package-key serialization is a later package,
// and this foundation never accepts a legacy raw-node component layout.
struct BlockyRigComponent
{
    std::shared_ptr<const BlockyRig> rig;
};

// Runtime-only root mapping. EntityRef avoids retaining recycled EnTT handles
// after direct child destruction or world teardown.
struct BlockyRigInstanceComponent
{
    std::shared_ptr<const BlockyRig> rig;
    std::vector<EntityRef> node_entities;
    bool spawned = false;
};

// Runtime marker for a generated limb entity. It is not serialized: the
// lifecycle path recreates the entire subtree from the immutable rig binding.
struct BlockyRigNodeComponent
{
    EntityRef rig_root;
    uint32_t node_index = 0;
};
