#pragma once

#include "Animation/BlockyRig.hpp"
#include "Components/IdentityComponent.hpp"
#include "EngineExport.h"
#include "world.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class world;

// Stable, ECS-independent transform data for a blocky-rig root. The rig API
// deliberately accepts value data and EntityRef handles rather than exposing
// the ECS implementation or its entity identifiers.
struct BlockyRigTransform
{
    glm::vec3 position{0.0f};
    glm::vec3 rotation_degrees{0.0f};
    glm::vec3 scale{1.0f};
};

// Read-only runtime information returned through the public rig API. The
// entity identity remains stable across implementation changes.
struct BlockyRigNodeState
{
    EntityRef entity{};
    glm::vec3 local_position{0.0f};
    glm::mat4 world_matrix{1.0f};
    glm::mat4 render_world_matrix{1.0f};
};

struct BlockyRigInstance
{
    std::vector<EntityRef> node_entities;
    bool spawned = false;
};

// Failure retains all construction ownership until resumeCleanup acknowledges it.
enum class BlockyRigStatus { Completed, InvalidInput, ConstructionFailure };
struct BlockyRigResult
{
    BlockyRigStatus status = BlockyRigStatus::Completed;
    EntityRef root{};
    ScopedRestoreStatus creation_status = ScopedRestoreStatus::Completed;
    LifecycleResult creation_rollback{};
    LifecycleResult cleanup{};
    std::vector<garden::ecs::EntityHandle> captured;
    bool cleanup_started = false;
};

namespace BlockyRigSystem
{
// Creates a generic root entity and binds exactly one immutable current-format
// rig resource. Node entities are created only by spawn/update.
ENGINE_API BlockyRigResult createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                                std::string_view tag = "BlockyRig");

// Binds a resource before nodes exist. Rebinding a spawned root is refused;
// callers must explicitly destroy the root and create another one.
ENGINE_API bool bind(world& game_world, EntityRef rig_root,
                     std::shared_ptr<const BlockyRig> rig);

ENGINE_API BlockyRigResult spawn(world& game_world, EntityRef rig_root);
ENGINE_API BlockyRigResult update(world& game_world);
// Retry and acknowledge construction rollback and subsequent cohort cleanup.
ENGINE_API bool resumeCleanup(world& game_world, BlockyRigResult& result);

// Supplies transient local-pose data for the next update. Invalid pose data
// is contained by the implementation and falls back to immutable rest data.
ENGINE_API bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                             bool pose_valid = true);
ENGINE_API bool setRootTransform(world& game_world, EntityRef rig_root,
                                 const BlockyRigTransform& transform);

ENGINE_API std::optional<BlockyRigInstance> instance(const world& game_world,
                                                     EntityRef rig_root);
ENGINE_API std::optional<BlockyRigNodeState> nodeState(const world& game_world,
                                                       EntityRef node);

// Explicit lifecycle operations for generated limbs and roots. Destroying one
// node never recreates it on update; destroying a root cascades its rig tree.
ENGINE_API LifecycleResult destroyNode(world& game_world, EntityRef node);
ENGINE_API LifecycleResult destroyRoot(world& game_world, EntityRef rig_root);
} // namespace BlockyRigSystem
