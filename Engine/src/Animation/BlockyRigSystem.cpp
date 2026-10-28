#include "Animation/BlockyRigSystem.hpp"

#include "Animation/BlockyRigEcsBridge.hpp"

namespace BlockyRigSystem
{
EntityRef createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                     std::string_view tag)
{
    return BlockyRigEcsBridge::createRoot(game_world, std::move(rig), tag);
}

bool bind(world& game_world, EntityRef rig_root, std::shared_ptr<const BlockyRig> rig)
{
    return BlockyRigEcsBridge::bind(game_world, rig_root, std::move(rig));
}

bool spawn(world& game_world, EntityRef rig_root)
{
    return BlockyRigEcsBridge::spawn(game_world, rig_root);
}

void update(world& game_world)
{
    BlockyRigEcsBridge::update(game_world);
}

bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                  bool pose_valid)
{
    return BlockyRigEcsBridge::setLocalPose(game_world, rig_root, local_pose, pose_valid);
}

bool setRootTransform(world& game_world, EntityRef rig_root,
                      const BlockyRigTransform& transform)
{
    return BlockyRigEcsBridge::setRootTransform(game_world, rig_root, transform);
}

std::optional<BlockyRigInstance> instance(const world& game_world, EntityRef rig_root)
{
    return BlockyRigEcsBridge::instance(game_world, rig_root);
}

std::optional<BlockyRigNodeState> nodeState(const world& game_world, EntityRef node)
{
    return BlockyRigEcsBridge::nodeState(game_world, node);
}

bool destroyNode(world& game_world, EntityRef node)
{
    return BlockyRigEcsBridge::destroyNode(game_world, node);
}

bool destroyRoot(world& game_world, EntityRef rig_root)
{
    return BlockyRigEcsBridge::destroyRoot(game_world, rig_root);
}
} // namespace BlockyRigSystem
