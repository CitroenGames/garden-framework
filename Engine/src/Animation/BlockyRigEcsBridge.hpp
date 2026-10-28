#pragma once

#include "Animation/BlockyRigSystem.hpp"

namespace BlockyRigEcsBridge
{
EntityRef createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                     std::string_view tag);
bool bind(world& game_world, EntityRef rig_root, std::shared_ptr<const BlockyRig> rig);
bool spawn(world& game_world, EntityRef rig_root);
void update(world& game_world);
bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                  bool pose_valid);
bool setRootTransform(world& game_world, EntityRef rig_root,
                      const BlockyRigTransform& transform);
std::optional<BlockyRigInstance> instance(const world& game_world, EntityRef rig_root);
std::optional<BlockyRigNodeState> nodeState(const world& game_world, EntityRef node);
bool destroyNode(world& game_world, EntityRef node);
bool destroyRoot(world& game_world, EntityRef rig_root);
} // namespace BlockyRigEcsBridge
