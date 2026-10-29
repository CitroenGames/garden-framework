#include "Animation/BlockyRigEcsBridge.hpp"

#include "Animation/BlockyRig.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Components/BlockyRigComponent.hpp"
#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "ECS/Hierarchy.hpp"
#include "world.hpp"

#include <cmath>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
bool finite(float value)
{
    return std::isfinite(value);
}

bool usablePose(const BonePose& pose)
{
    constexpr float minimum_scale = 1e-8f;
    const float rotation_length = glm::length(pose.rotation);
    return finite(pose.translation.x) && finite(pose.translation.y) && finite(pose.translation.z) &&
           finite(pose.scale.x) && finite(pose.scale.y) && finite(pose.scale.z) &&
           finite(pose.rotation.w) && finite(pose.rotation.x) && finite(pose.rotation.y) &&
           finite(pose.rotation.z) && finite(rotation_length) && rotation_length > minimum_scale &&
           std::abs(pose.scale.x) > minimum_scale && std::abs(pose.scale.y) > minimum_scale &&
           std::abs(pose.scale.z) > minimum_scale;
}

glm::mat4 pivotedMatrix(const BlockyRigNodeDefinition& node, const BonePose& pose)
{
    return glm::translate(glm::mat4(1.0f), node.pivot) * pose.toMatrix() *
           glm::translate(glm::mat4(1.0f), -node.pivot);
}

bool writeNodeLocal(entt::registry& registry, entt::entity node_entity,
                    const BlockyRigNodeDefinition& node, const BonePose& candidate)
{
    if (node_entity == entt::null || !registry.valid(node_entity))
        return false;
    auto& transform = registry.get_or_emplace<TransformComponent>(node_entity);
    const BonePose& safe_pose = usablePose(candidate) ? candidate : node.rest_pose;
    if (!Hierarchy::decomposeToTransformComponent(pivotedMatrix(node, safe_pose), transform))
    {
        // Validated immutable rest data is the sole recovery path for a bad
        // transient pose. It never consults old assets or a previous frame.
        if (!Hierarchy::decomposeToTransformComponent(pivotedMatrix(node, node.rest_pose), transform))
            return false;
    }
    Hierarchy::markDirty(registry, node_entity);
    return true;
}

bool finishOperation(world& owner, LifecycleResult& result)
{
    if (!result.operation.valid()) return result.cleanupComplete();
    if (!result.cleanupComplete()) {
        auto progress = owner.resumeRetirement(result.operation);
        if (!progress.operation.valid() && !progress.cleanupComplete()) return false;
        if (progress.operation.valid()) result = progress;
    }
    if (!result.cleanupComplete()) return false;
    if (!owner.acknowledgeRetirement(result.operation).cleanupComplete()) return false;
    result = {};
    return true;
}

BlockyRigResult spawnImpl(world& game_world, entt::entity rig_root)
{
    BlockyRigResult result;
    auto& registry = game_world.registry;
    if (rig_root == entt::null || !registry.valid(rig_root)) {
        result.status = BlockyRigStatus::InvalidInput; return result;
    }
    const auto* binding = registry.try_get<BlockyRigComponent>(rig_root);
    if (!binding || !binding->rig || !registry.all_of<IdentityComponent>(rig_root)) {
        result.status = BlockyRigStatus::InvalidInput; return result;
    }
    if (const auto* existing = registry.try_get<BlockyRigInstanceComponent>(rig_root)) {
        if (!existing->spawned || existing->rig != binding->rig)
            result.status = BlockyRigStatus::InvalidInput;
        return result;
    }
    const auto rig = binding->rig;
    const auto& nodes = rig->nodes();
    const EntityRef root_ref = entityRefOf(registry, rig_root);
    if (nodes.empty() || root_ref.isNull()) {
        result.status = BlockyRigStatus::InvalidInput; return result;
    }
    result.root = root_ref;
    try {
        std::vector<entt::entity> created;
        created.reserve(nodes.size());
        result.captured.reserve(nodes.size());
        std::vector<EntityRef> node_refs(nodes.size());
        registry.get_or_emplace<TransformComponent>(rig_root);
        registry.get_or_emplace<WorldTransformComponent>(rig_root);
        for (size_t index = 0; index < nodes.size(); ++index) {
            const auto creation = game_world.createEntity("BlockyRigNode");
            if (creation.status != ScopedRestoreStatus::Completed) {
                result.creation_status = creation.status;
                result.creation_rollback = creation.rollback;
                result.status = BlockyRigStatus::ConstructionFailure;
                return result;
            }
            result.captured.push_back(creation.entity);
            const entt::entity node_entity = resolveEntityRef(registry, creation.reference);
            if (node_entity == entt::null) {
                result.status = BlockyRigStatus::ConstructionFailure; return result;
            }
            created.push_back(node_entity);
            registry.emplace<TransformComponent>(node_entity);
            registry.emplace<WorldTransformComponent>(node_entity);
            registry.emplace<BlockyRigNodeComponent>(node_entity,
                BlockyRigNodeComponent{root_ref, static_cast<uint32_t>(index)});
            registry.emplace<HierarchyPolicyComponent>(node_entity,
                HierarchyPolicyComponent{HierarchyDestroyPolicy::CascadeDestroy});
            const int parent_index = nodes[index].parent_index;
            const entt::entity parent = parent_index < 0 ? rig_root : created[static_cast<size_t>(parent_index)];
            if (!Hierarchy::attach(registry, node_entity, parent, false) ||
                !writeNodeLocal(registry, node_entity, nodes[index], nodes[index].rest_pose)) {
                result.status = BlockyRigStatus::ConstructionFailure; return result;
            }
            node_refs[index] = creation.reference;
        }
        registry.emplace_or_replace<HierarchyPolicyComponent>(rig_root,
            HierarchyPolicyComponent{HierarchyDestroyPolicy::CascadeDestroy});
        registry.emplace_or_replace<BlockyRigInstanceComponent>(rig_root,
            BlockyRigInstanceComponent{rig, std::move(node_refs), true});
        Hierarchy::markDirty(registry, rig_root);
        result.captured.clear();
    } catch (...) {
        result.status = BlockyRigStatus::ConstructionFailure;
    }
    return result;
}

BlockyRigResult updateImpl(world& game_world)
{
    auto& registry = game_world.registry;
    std::vector<entt::entity> roots;
    for (const entt::entity root : registry.view<BlockyRigComponent>())
        roots.push_back(root);
    for (const entt::entity root : roots)
    {
        if (!registry.valid(root))
            continue;
        const auto& binding = registry.get<BlockyRigComponent>(root);
        if (!binding.rig)
            continue;
        if (!registry.all_of<BlockyRigInstanceComponent>(root)) {
            auto result = spawnImpl(game_world, root);
            if (result.status != BlockyRigStatus::Completed) return result;
        }

        auto* instance = registry.try_get<BlockyRigInstanceComponent>(root);
        if (!instance || !instance->spawned || instance->rig != binding.rig)
            continue;
        const auto& nodes = binding.rig->nodes();
        if (instance->node_entities.size() != nodes.size())
            continue;
        const auto* pose = registry.try_get<AnimPoseComponent>(root);
        const bool pose_matches = pose && pose->pose_valid &&
            pose->local_pose.getBoneCount() == static_cast<int>(nodes.size());

        for (size_t index = 0; index < nodes.size(); ++index)
        {
            EntityRef& node_ref = instance->node_entities[index];
            const entt::entity node_entity = resolveEntityRef(registry, node_ref);
            if (node_entity == entt::null)
            {
                node_ref = {};
                continue;
            }
            const BonePose& desired = pose_matches ? pose->local_pose[static_cast<int>(index)] :
                                                    nodes[index].rest_pose;
            (void)writeNodeLocal(registry, node_entity, nodes[index], desired);
        }
    }
    Hierarchy::updateTransformPropagation(registry);
    return {};
}
} // namespace

namespace BlockyRigEcsBridge
{
BlockyRigResult createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                           std::string_view tag)
{
    BlockyRigResult result;
    if (!rig) { result.status = BlockyRigStatus::InvalidInput; return result; }
    try { result.captured.reserve(1); }
    catch (...) { result.status = BlockyRigStatus::ConstructionFailure; return result; }
    const auto creation = game_world.createEntity(tag);
    result.creation_status = creation.status;
    if (creation.status != ScopedRestoreStatus::Completed) {
        result.status = BlockyRigStatus::ConstructionFailure;
        result.creation_rollback = creation.rollback;
        return result;
    }
    result.captured.push_back(creation.entity);
    result.root = creation.reference;
    try {
        const entt::entity root = resolveEntityRef(game_world.registry, result.root);
        if (root == entt::null) { result.status = BlockyRigStatus::ConstructionFailure; return result; }
        game_world.registry.emplace<BlockyRigComponent>(root, BlockyRigComponent{std::move(rig)});
        game_world.registry.emplace<TransformComponent>(root);
        game_world.registry.emplace<WorldTransformComponent>(root);
        result.captured.clear();
    } catch (...) { result.status = BlockyRigStatus::ConstructionFailure; }
    return result;
}

bool resumeCleanup(world& game_world, BlockyRigResult& result)
{
    if (!finishOperation(game_world, result.creation_rollback)) return false;
    if (!result.cleanup_started && !result.captured.empty()) {
        result.cleanup = game_world.retireEntities(result.captured);
        if (!result.cleanup.accepted) return false;
        result.cleanup_started = true;
    }
    if (result.cleanup_started && !finishOperation(game_world, result.cleanup)) return false;
    result.captured.clear();
    result.cleanup_started = false;
    return true;
}

bool bind(world& game_world, EntityRef rig_root, std::shared_ptr<const BlockyRig> rig)
{
    if (rig_root.isNull() || !rig)
        return false;

    const entt::entity root = resolveEntityRef(game_world.registry, rig_root);
    if (root == entt::null || game_world.registry.all_of<BlockyRigInstanceComponent>(root))
        return false;

    game_world.registry.emplace_or_replace<BlockyRigComponent>(
        root, BlockyRigComponent{std::move(rig)});
    return true;
}

BlockyRigResult spawn(world& game_world, EntityRef rig_root)
{
    return spawnImpl(game_world, resolveEntityRef(game_world.registry, rig_root));
}

BlockyRigResult update(world& game_world)
{
    return updateImpl(game_world);
}

bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                  bool pose_valid)
{
    const entt::entity root = resolveEntityRef(game_world.registry, rig_root);
    if (root == entt::null || !game_world.registry.all_of<BlockyRigComponent>(root))
        return false;

    auto& pose = game_world.registry.get_or_emplace<AnimPoseComponent>(root);
    pose.local_pose = local_pose;
    pose.pose_valid = pose_valid;
    return true;
}

bool setRootTransform(world& game_world, EntityRef rig_root,
                      const BlockyRigTransform& transform)
{
    const entt::entity root = resolveEntityRef(game_world.registry, rig_root);
    if (root == entt::null || !game_world.registry.all_of<BlockyRigComponent>(root))
        return false;

    auto& target = game_world.registry.get_or_emplace<TransformComponent>(root);
    target.position = transform.position;
    target.rotation = transform.rotation_degrees;
    target.scale = transform.scale;
    Hierarchy::markDirty(game_world.registry, root);
    return true;
}

std::optional<BlockyRigInstance> instance(const world& game_world, EntityRef rig_root)
{
    const entt::entity root = resolveEntityRef(game_world.registry, rig_root);
    const auto* value = root == entt::null ? nullptr :
        game_world.registry.try_get<BlockyRigInstanceComponent>(root);
    if (!value)
        return std::nullopt;
    return BlockyRigInstance{value->node_entities, value->spawned};
}

std::optional<BlockyRigNodeState> nodeState(const world& game_world, EntityRef node)
{
    const entt::entity entity = resolveEntityRef(game_world.registry, node);
    if (entity == entt::null || !game_world.registry.all_of<BlockyRigNodeComponent>(entity))
        return std::nullopt;

    const auto* transform = game_world.registry.try_get<TransformComponent>(entity);
    const auto* world_transform = game_world.registry.try_get<WorldTransformComponent>(entity);
    const auto render_matrix = Hierarchy::resolveRenderWorldMatrix(game_world.registry, entity);
    if (!transform || !world_transform || !render_matrix)
        return std::nullopt;
    return BlockyRigNodeState{node, transform->position, world_transform->matrix, *render_matrix};
}

LifecycleResult destroyNode(world& game_world, EntityRef node)
{
    const entt::entity entity = resolveEntityRef(game_world.registry, node);
    if (entity == entt::null || !game_world.registry.all_of<BlockyRigNodeComponent>(entity))
        return {LifecycleStatus::Invalid};
    const auto handle = game_world.scopedEntity(node);
    return game_world.retireEntities(std::span{&handle, 1});
}

LifecycleResult destroyRoot(world& game_world, EntityRef rig_root)
{
    const entt::entity root = resolveEntityRef(game_world.registry, rig_root);
    if (root == entt::null || !game_world.registry.all_of<BlockyRigComponent>(root))
        return {LifecycleStatus::Invalid};
    const auto handle = game_world.scopedEntity(rig_root);
    return game_world.retireEntities(std::span{&handle, 1});
}
} // namespace BlockyRigEcsBridge
