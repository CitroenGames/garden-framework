#include "Animation/BlockyRigEcsBridge.hpp"

#include "Animation/BlockyRig.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Components/BlockyRigComponent.hpp"
#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "ECS/EntityLifecycle.hpp"
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

void destroyCreated(EntityLifecycle& lifecycle, const std::vector<entt::entity>& created)
{
    for (auto it = created.rbegin(); it != created.rend(); ++it)
        lifecycle.destroyImmediate(*it);
}
} // namespace

namespace
{
bool spawnImpl(entt::registry& registry, EntityLifecycle& lifecycle, entt::entity rig_root)
{
    if (rig_root == entt::null || !registry.valid(rig_root))
        return false;
    const auto* binding = registry.try_get<BlockyRigComponent>(rig_root);
    if (!binding || !binding->rig)
        return false;
    if (!registry.all_of<IdentityComponent>(rig_root))
        return false;

    if (const auto* existing = registry.try_get<BlockyRigInstanceComponent>(rig_root))
    {
        // A completed instance belongs to exactly one immutable resource. A
        // resource replacement must use an explicit subtree teardown first.
        return existing->spawned && existing->rig == binding->rig;
    }

    const auto& nodes = binding->rig->nodes();
    if (nodes.empty())
        return false;

    std::vector<entt::entity> created;
    created.reserve(nodes.size());
    std::vector<EntityRef> node_refs(nodes.size());
    const EntityRef root_ref = lifecycle.refOf(rig_root);
    if (root_ref.isNull())
        return false;

    registry.get_or_emplace<TransformComponent>(rig_root);
    registry.get_or_emplace<WorldTransformComponent>(rig_root);
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const entt::entity node_entity = lifecycle.create("BlockyRigNode");
        created.push_back(node_entity);
        registry.emplace<TransformComponent>(node_entity);
        registry.emplace<WorldTransformComponent>(node_entity);
        registry.emplace<BlockyRigNodeComponent>(node_entity,
            BlockyRigNodeComponent{root_ref, static_cast<uint32_t>(index)});
        registry.emplace<HierarchyPolicyComponent>(
            node_entity, HierarchyPolicyComponent{HierarchyDestroyPolicy::CascadeDestroy});

        const int parent_index = nodes[index].parent_index;
        const entt::entity parent = parent_index < 0 ? rig_root :
            created[static_cast<size_t>(parent_index)];
        if (!Hierarchy::attach(registry, node_entity, parent, false) ||
            !writeNodeLocal(registry, node_entity, nodes[index], nodes[index].rest_pose))
        {
            destroyCreated(lifecycle, created);
            return false;
        }
        node_refs[index] = lifecycle.refOf(node_entity);
    }

    registry.emplace_or_replace<HierarchyPolicyComponent>(
        rig_root, HierarchyPolicyComponent{HierarchyDestroyPolicy::CascadeDestroy});
    registry.emplace_or_replace<BlockyRigInstanceComponent>(rig_root,
        BlockyRigInstanceComponent{binding->rig, std::move(node_refs), true});
    Hierarchy::markDirty(registry, rig_root);
    return true;
}

void updateImpl(entt::registry& registry, EntityLifecycle& lifecycle)
{
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
        if (!registry.all_of<BlockyRigInstanceComponent>(root))
            (void)spawnImpl(registry, lifecycle, root);

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
            const entt::entity node_entity = lifecycle.resolve(node_ref);
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
}
} // namespace

namespace BlockyRigEcsBridge
{
EntityRef createRoot(world& game_world, std::shared_ptr<const BlockyRig> rig,
                     std::string_view tag)
{
    if (!rig)
        return {};

    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity root = lifecycle.create(tag);
    game_world.registry.emplace<BlockyRigComponent>(root, BlockyRigComponent{std::move(rig)});
    game_world.registry.emplace<TransformComponent>(root);
    game_world.registry.emplace<WorldTransformComponent>(root);
    return lifecycle.refOf(root);
}

bool bind(world& game_world, EntityRef rig_root, std::shared_ptr<const BlockyRig> rig)
{
    if (rig_root.isNull() || !rig)
        return false;

    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity root = lifecycle.resolve(rig_root);
    if (root == entt::null || game_world.registry.all_of<BlockyRigInstanceComponent>(root))
        return false;

    game_world.registry.emplace_or_replace<BlockyRigComponent>(
        root, BlockyRigComponent{std::move(rig)});
    return true;
}

bool spawn(world& game_world, EntityRef rig_root)
{
    EntityLifecycle lifecycle(game_world.registry);
    return spawnImpl(game_world.registry, lifecycle, lifecycle.resolve(rig_root));
}

void update(world& game_world)
{
    EntityLifecycle lifecycle(game_world.registry);
    updateImpl(game_world.registry, lifecycle);
}

bool setLocalPose(world& game_world, EntityRef rig_root, const Pose& local_pose,
                  bool pose_valid)
{
    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity root = lifecycle.resolve(rig_root);
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
    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity root = lifecycle.resolve(rig_root);
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

bool destroyNode(world& game_world, EntityRef node)
{
    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity entity = lifecycle.resolve(node);
    if (entity == entt::null || !game_world.registry.all_of<BlockyRigNodeComponent>(entity))
        return false;
    lifecycle.destroyImmediate(entity);
    return true;
}

bool destroyRoot(world& game_world, EntityRef rig_root)
{
    EntityLifecycle lifecycle(game_world.registry);
    const entt::entity root = lifecycle.resolve(rig_root);
    if (root == entt::null || !game_world.registry.all_of<BlockyRigComponent>(root))
        return false;
    lifecycle.destroyImmediate(root);
    return true;
}
} // namespace BlockyRigEcsBridge
