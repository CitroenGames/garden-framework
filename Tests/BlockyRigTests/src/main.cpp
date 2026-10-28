#include "Animation/BlockyRig.hpp"
#include "Animation/BlockyRigSystem.hpp"
#include "Components/Components.hpp"
#include "world.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
bool near(float actual, float expected, float epsilon = 1e-4f)
{
    return std::abs(actual - expected) <= epsilon;
}

bool nearVec(const glm::vec3& actual, const glm::vec3& expected, float epsilon = 1e-4f)
{
    return near(actual.x, expected.x, epsilon) && near(actual.y, expected.y, epsilon) &&
           near(actual.z, expected.z, epsilon);
}

bool nearMat(const glm::mat4& actual, const glm::mat4& expected, float epsilon = 1e-4f)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!near(actual[column][row], expected[column][row], epsilon)) return false;
    return true;
}

bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << '\n';
    return false;
}

bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << '\n';
    return true;
}

BlockyRigNodeDefinition node(std::string name, int parent, glm::vec3 position,
                             glm::vec3 pivot = glm::vec3(0.0f))
{
    BlockyRigNodeDefinition result;
    result.name = std::move(name);
    result.parent_index = parent;
    result.pivot = pivot;
    result.rest_pose.translation = position;
    result.mesh_slot = result.name;
    return result;
}

std::shared_ptr<const BlockyRig> makeRig()
{
    BlockyRigDefinition definition;
    definition.source_signature = "BlockyRigTests/v1";
    definition.nodes = {
        node("root", -1, {0.0f, 0.0f, 0.0f}),
        node("arm", 0, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
        node("hand", 1, {0.0f, 1.0f, 0.0f}),
        node("other", 0, {0.0f, -1.0f, 0.0f}),
    };
    BlockyRigError error;
    return BlockyRigValidation::make(std::move(definition), error);
}

bool testCurrentOnlyValidation()
{
    const std::string name = "Blocky.CurrentOnlyValidation";
    BlockyRigDefinition definition;
    definition.source_signature = "rig";
    definition.nodes = {node("root", -1, {0.0f, 0.0f, 0.0f})};
    BlockyRigError error;

    definition.format_version = 0;
    if (BlockyRigValidation::make(definition, error) || error.code != "BLOCKY_RIG_VERSION")
        return fail(name, "retired rig version was accepted");
    definition.format_version = BlockyRigDefinition::CurrentFormatVersion + 1;
    if (BlockyRigValidation::make(definition, error) || error.code != "BLOCKY_RIG_VERSION")
        return fail(name, "future rig version was accepted");
    definition.format_version = BlockyRigDefinition::CurrentFormatVersion;
    definition.nodes.push_back(node("root", 0, {0.0f, 0.0f, 0.0f}));
    if (BlockyRigValidation::make(definition, error) || error.code != "BLOCKY_RIG_NODE_NAME")
        return fail(name, "duplicate node name was accepted");
    definition.nodes.back().name = "late";
    definition.nodes.back().parent_index = 1;
    if (BlockyRigValidation::make(definition, error) || error.code != "BLOCKY_RIG_PARENT_ORDER")
        return fail(name, "cyclic or unordered parent was accepted");
    return pass(name);
}

bool testPoseReachesHierarchyAndRenderMatrix()
{
    const std::string name = "Blocky.PivotHierarchyRender";
    const auto rig = makeRig();
    if (!rig) return fail(name, "fixture rig was rejected");

    world game_world;
    const EntityRef root = BlockyRigSystem::createRoot(game_world, rig, "RigRoot");
    const BlockyRigTransform root_transform{{0.0f, 0.0f, 0.0f}, {0.0f, 90.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    if (root.isNull() || !BlockyRigSystem::setRootTransform(game_world, root, root_transform))
        return fail(name, "root setup failed through world API");

    Pose pose;
    pose.resize(4);
    pose[1].rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    if (!BlockyRigSystem::setLocalPose(game_world, root, pose))
        return fail(name, "pose setup failed through world API");
    BlockyRigSystem::update(game_world);

    const auto instance = BlockyRigSystem::instance(game_world, root);
    if (!instance || !instance->spawned || instance->node_entities.size() != 4)
        return fail(name, "rig children were not created through lifecycle");
    const EntityRef arm = instance->node_entities[1];
    const EntityRef hand = instance->node_entities[2];
    const auto arm_state = BlockyRigSystem::nodeState(game_world, arm);
    const auto hand_state = BlockyRigSystem::nodeState(game_world, hand);
    if (!arm_state || !hand_state)
        return fail(name, "node references did not resolve through world API");

    const glm::mat4 pivoted_arm =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
        pose[1].toMatrix() *
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    TransformComponent root_component;
    root_component.rotation = root_transform.rotation_degrees;
    const glm::mat4 expected = root_component.getTransformMatrix() * pivoted_arm *
                               pose[2].toMatrix();
    if (!nearMat(hand_state->world_matrix, expected) ||
        !nearMat(hand_state->render_world_matrix, expected))
        return fail(name, "local pose did not reach matching hierarchy and render matrices");
    if (!nearVec(arm_state->local_position, {1.0f, 1.0f, 0.0f}))
        return fail(name, "pivot rotation did not move the limb around its pivot");
    return pass(name);
}

bool testInvalidPoseAndDestroyedChild()
{
    const std::string name = "Blocky.InvalidPoseAndDestroyedChild";
    const auto rig = makeRig();
    if (!rig) return fail(name, "fixture rig was rejected");

    world game_world;
    const EntityRef root = BlockyRigSystem::createRoot(game_world, rig, "RigRoot");
    Pose pose;
    pose.resize(4);
    if (root.isNull() || !BlockyRigSystem::setLocalPose(game_world, root, pose))
        return fail(name, "root or pose setup failed through world API");
    BlockyRigSystem::update(game_world);

    const auto initial = BlockyRigSystem::instance(game_world, root);
    if (!initial || initial->node_entities.size() != 4)
        return fail(name, "rig instance was unavailable through world API");
    const EntityRef arm = initial->node_entities[1];
    const EntityRef other = initial->node_entities[3];
    pose[3].translation = {3.0f, 0.0f, 0.0f};
    pose[1].translation.x = std::numeric_limits<float>::quiet_NaN();
    if (!BlockyRigSystem::setLocalPose(game_world, root, pose))
        return fail(name, "updated pose was rejected");
    BlockyRigSystem::update(game_world);

    const auto arm_state = BlockyRigSystem::nodeState(game_world, arm);
    const auto other_state = BlockyRigSystem::nodeState(game_world, other);
    if (!arm_state || !nearVec(arm_state->local_position, {0.0f, 0.0f, 0.0f}))
        return fail(name, "non-finite pose did not fall back to immutable rest pose");
    if (!other_state || !nearVec(other_state->local_position, {3.0f, 0.0f, 0.0f}))
        return fail(name, "valid sibling pose was not applied");

    if (!BlockyRigSystem::destroyNode(game_world, arm))
        return fail(name, "generated child could not be explicitly destroyed");
    BlockyRigSystem::update(game_world);
    const auto after = BlockyRigSystem::instance(game_world, root);
    if (!after || !after->node_entities[1].isNull())
        return fail(name, "destroyed child did not become an empty node mapping");
    if (!BlockyRigSystem::nodeState(game_world, other))
        return fail(name, "destroying one limb destroyed an unrelated sibling");

    if (!BlockyRigSystem::destroyRoot(game_world, root) ||
        BlockyRigSystem::instance(game_world, root) ||
        BlockyRigSystem::nodeState(game_world, other))
        return fail(name, "root destruction did not cascade through generated rig subtree");
    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    ok = testCurrentOnlyValidation() && ok;
    ok = testPoseReachesHierarchyAndRenderMatrix() && ok;
    ok = testInvalidPoseAndDestroyedChild() && ok;
    return ok ? 0 : 1;
}
