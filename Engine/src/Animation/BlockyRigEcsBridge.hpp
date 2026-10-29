#pragma once

#include "Animation/BlockyRigSystem.hpp"

namespace BlockyRigEcsBridge
{
BlockyRigResult createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                     std::string_view tag);
bool bind(world& game_world, EntityRef rig_root, std::shared_ptr<const BlockyRig> rig);
BlockyRigResult spawn(world& game_world, EntityRef rig_root);
BlockyRigResult update(world& game_world);
bool resumeCleanup(world& game_world, BlockyRigResult& result);
bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                  bool pose_valid);
bool setRootTransform(world& game_world, EntityRef rig_root,
                      const BlockyRigTransform& transform);
std::optional<BlockyRigInstance> instance(const world& game_world, EntityRef rig_root);
std::optional<BlockyRigNodeState> nodeState(const world& game_world, EntityRef node);
LifecycleResult destroyNode(world& game_world, EntityRef node);
LifecycleResult destroyRoot(world& game_world, EntityRef rig_root);
} // namespace BlockyRigEcsBridge
