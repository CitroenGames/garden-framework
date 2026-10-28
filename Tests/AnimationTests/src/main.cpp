#define GLM_ENABLE_EXPERIMENTAL

#include "Animation/AnimationComponentMigration.hpp"
#include "Animation/AnimationClip.hpp"
#include "Animation/AnimationSampling.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Animation/IKSolver.hpp"
#include "Animation/Pose.hpp"
#include "Animation/Skeleton.hpp"
#include "Components/AnimPlaybackComponent.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Components/IKComponent.hpp"
#include "Components/SkeletonComponent.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"

#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace
{
bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

bool near(float a, float b, float epsilon = 0.001f)
{
    return std::abs(a - b) <= epsilon;
}

bool nearVec(const glm::vec3& a, const glm::vec3& b, float epsilon = 0.001f)
{
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon);
}

bool nearPose(const Pose& a, const Pose& b, float epsilon = 0.001f)
{
    if (a.getBoneCount() != b.getBoneCount())
        return false;
    for (int index = 0; index < a.getBoneCount(); ++index)
    {
        if (!nearVec(a[index].translation, b[index].translation, epsilon) ||
            !nearVec(a[index].scale, b[index].scale, epsilon) ||
            std::abs(glm::dot(a[index].rotation, b[index].rotation)) < 1.0f - epsilon)
            return false;
    }
    return true;
}

bool nearMatrices(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b,
                  float epsilon = 0.001f)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!near(a[i][column][row], b[i][column][row], epsilon)) return false;
    return true;
}

Bone makeBone(const char* name, int parent, const glm::vec3& translation,
              const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
              const glm::vec3& scale = glm::vec3(1.0f))
{
    Bone bone;
    bone.name = name;
    bone.parent_id = parent;
    bone.local_transform = glm::translate(glm::mat4(1.0f), translation) * glm::toMat4(rotation) *
        glm::scale(glm::mat4(1.0f), scale);
    return bone;
}

std::shared_ptr<const Skeleton> makeRestSkeleton()
{
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->addBone(makeBone("root", -1, {3.0f, -2.0f, 1.0f}, glm::angleAxis(0.4f, glm::vec3(0, 0, 1)), {1.5f, 1.0f, 0.75f}));
    skeleton->addBone(makeBone("mid", 0, {1.0f, 2.0f, 0.5f}, glm::angleAxis(-0.2f, glm::vec3(0, 1, 0)), {0.8f, 1.2f, 1.0f}));
    skeleton->addBone(makeBone("end", 1, {-0.5f, 0.25f, 2.0f}, glm::angleAxis(0.3f, glm::vec3(1, 0, 0)), {1.0f, 0.9f, 1.1f}));
    return skeleton;
}

std::shared_ptr<const AnimationResourceSet> makeResources(
    std::shared_ptr<const Skeleton> skeleton,
    std::unordered_map<std::string, std::shared_ptr<const AnimationClip>> clips = {})
{
    return std::make_shared<AnimationResourceSet>(std::move(skeleton), std::move(clips));
}

std::shared_ptr<AnimationClip> makeChainClip(const char* name = "chain")
{
    auto clip = std::make_shared<AnimationClip>();
    clip->name = name;
    clip->duration = 2.0f;
    for (int index = 0; index < 3; ++index)
    {
        BoneAnimation channel;
        channel.bone_index = index;
        channel.position_keys = {{0.0f, {0.0f, 0.0f, 0.0f}}, {2.0f, {0.0f, 0.0f, 0.0f}}};
        channel.rotation_keys = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
        channel.scale_keys = {{0.0f, {1.0f, 1.0f, 1.0f}}};
        clip->channels.push_back(std::move(channel));
    }
    return clip;
}

bool testRestPoseAndOverlay()
{
    const std::string name = "AN-1 pose sampling preserves skeleton rest until authored keys override it";
    const auto skeleton = makeRestSkeleton();
    Pose rest;
    AnimationSampling::makeRestPose(*skeleton, rest);

    AnimationClip empty;
    Pose empty_result = rest;
    AnimationSampling::overlayClip(empty, 0.0f, empty_result);
    if (!nearPose(empty_result, rest))
        return fail(name, "empty clip did not preserve every non-identity rest transform");

    AnimationClip partial;
    BoneAnimation channel;
    channel.bone_index = 1;
    channel.position_keys = {{0.0f, {9.0f, 8.0f, 7.0f}}};
    partial.channels.push_back(channel);
    Pose partial_result = rest;
    AnimationSampling::overlayClip(partial, 0.0f, partial_result);
    if (!nearVec(partial_result[1].translation, {9.0f, 8.0f, 7.0f}) ||
        std::abs(glm::dot(partial_result[1].rotation, rest[1].rotation)) < 0.999f ||
        !nearVec(partial_result[1].scale, rest[1].scale) ||
        !nearVec(partial_result[0].translation, rest[0].translation) ||
        std::abs(glm::dot(partial_result[0].rotation, rest[0].rotation)) < 0.999f ||
        !nearVec(partial_result[0].scale, rest[0].scale) ||
        !nearVec(partial_result[2].translation, rest[2].translation) ||
        std::abs(glm::dot(partial_result[2].rotation, rest[2].rotation)) < 0.999f ||
        !nearVec(partial_result[2].scale, rest[2].scale))
        return fail(name, "partial channel overwrote a missing property or unrelated bone");
    if (!nearVec(partial_result[0].translation, rest[0].translation) ||
        !nearVec(partial_result[2].translation, rest[2].translation))
        return fail(name, "omitted bones did not retain rest pose");

    AnimationClip explicit_identity;
    BoneAnimation identity;
    identity.bone_index = 0;
    identity.position_keys = {{0.0f, glm::vec3(0.0f)}};
    identity.rotation_keys = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
    identity.scale_keys = {{0.0f, glm::vec3(1.0f)}};
    explicit_identity.channels.push_back(identity);
    Pose identity_result = rest;
    AnimationSampling::overlayClip(explicit_identity, 0.0f, identity_result);
    if (!nearVec(identity_result[0].translation, glm::vec3(0.0f)) ||
        std::abs(glm::dot(identity_result[0].rotation, glm::quat(1, 0, 0, 0))) < 0.999f ||
        !nearVec(identity_result[0].scale, glm::vec3(1.0f)))
        return fail(name, "explicit identity keys did not produce identity");

    return pass(name);
}

bool testSystemRestAndInvalidPlayback()
{
    const std::string name = "AN-1 system creates a valid rest pose for stopped, absent, and invalid playback";
    const auto skeleton = makeRestSkeleton();
    Pose expected_rest;
    AnimationSampling::makeRestPose(*skeleton, expected_rest);

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& binding = registry.emplace<SkeletonComponent>(entity);
    binding.resources = makeResources(skeleton);
    binding.auto_play = true;
    binding.default_clip = "missing";
    AnimationSystem::update(registry, 0.0f, false);
    if (!registry.all_of<AnimPlaybackComponent, AnimPoseComponent>(entity))
        return fail(name, "runtime components were not initialized for a serialized skeleton binding");
    auto& playback = registry.get<AnimPlaybackComponent>(entity);
    auto& pose = registry.get<AnimPoseComponent>(entity);
    if (!pose.pose_valid || !nearPose(pose.local_pose, expected_rest) || !pose.bone_matrices.empty())
        return fail(name, "stopped binding did not evaluate rest or allocated a headless palette");

    playback.current_clip = std::make_shared<AnimationClip>();
    playback.playing = true;
    AnimationSystem::update(registry, 0.0f, false);
    if (!nearPose(pose.local_pose, expected_rest))
        return fail(name, "empty clip did not evaluate to rest pose");

    playback.current_clip = makeChainClip("invalid");
    playback.playing = true;
    playback.time = 0.0f;
    playback.speed = std::numeric_limits<float>::quiet_NaN();
    AnimationSystem::update(registry, 1.0f, false);
    if (!near(playback.time, 0.0f) || !pose.pose_valid)
        return fail(name, "non-finite playback speed advanced or invalidated the pose");
    playback.speed = 1.0f;
    AnimationSystem::update(registry, -1.0f, false);
    if (!near(playback.time, 0.0f))
        return fail(name, "negative delta advanced playback");
    return pass(name);
}

bool testIkOrder()
{
    const std::string name = "AN-1 ECS animation applies two-bone IK before FABRIK";
    const auto skeleton = makeRestSkeleton();
    const auto clip = makeChainClip();
    TwoBoneIKConstraint two_bone{0, 1, 2, {1.25f, 0.65f, 0.35f}, {0.0f, 0.0f, 1.0f}};
    FABRIKConstraint fabrik;
    fabrik.chain = {0, 1, 2}; fabrik.target = {0.25f, 1.35f, 0.75f}; fabrik.max_iterations = 16; fabrik.tolerance = 0.0001f;

    Pose expected;
    AnimationSampling::makeRestPose(*skeleton, expected);
    AnimationSampling::overlayClip(*clip, 0.0f, expected);
    std::vector<glm::mat4> globals;
    skeleton->computeGlobalTransforms(expected, globals);
    TwoBoneIKConstraint::solve(*skeleton, expected, globals, two_bone);
    FABRIKConstraint::solve(*skeleton, expected, globals, fabrik);
    std::vector<glm::mat4> expected_final;
    skeleton->computeFinalMatrices(expected, expected_final);

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& binding = registry.emplace<SkeletonComponent>(entity);
    binding.resources = makeResources(skeleton);
    auto& playback = registry.emplace<AnimPlaybackComponent>(entity);
    playback.initialized = true; playback.current_clip = clip; playback.playing = true;
    auto& ik = registry.emplace<IKComponent>(entity);
    ik.two_bone_constraints.push_back(two_bone); ik.fabrik_constraints.push_back(fabrik);
    AnimationSystem::update(registry, 0.0f);
    if (!nearMatrices(registry.get<AnimPoseComponent>(entity).bone_matrices, expected_final))
        return fail(name, "system output did not match the two-bone then FABRIK oracle");
    return pass(name);
}

int g_finished_events = 0;
std::string g_finished_clip;
void onAnimationFinished(const AnimationFinishedEvent& event) { ++g_finished_events; g_finished_clip = event.clip_name; }

bool testCompletionEvent()
{
    const std::string name = "AN-1 non-looping completion queues exactly one event";
    EventBus::get().clear();
    EventBus::get().sink<AnimationFinishedEvent>().connect<&onAnimationFinished>();
    g_finished_events = 0; g_finished_clip.clear();
    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& binding = registry.emplace<SkeletonComponent>(entity); binding.resources = makeResources(makeRestSkeleton());
    auto& playback = registry.emplace<AnimPlaybackComponent>(entity);
    playback.initialized = true;
    auto completion_clip = makeChainClip("complete_once");
    completion_clip->duration = 0.25f;
    playback.current_clip = completion_clip;
    playback.playing = true; playback.looping = false;
    AnimationSystem::update(registry, 0.5f);
    EventBus::get().flush<AnimationFinishedEvent>();
    Pose completed_rest;
    AnimationSampling::makeRestPose(*binding.resources->skeleton(), completed_rest);
    if (!nearPose(registry.get<AnimPoseComponent>(entity).local_pose, completed_rest))
        return fail(name, "completed playback did not return to the skeleton rest pose");
    AnimationSystem::update(registry, 0.5f);
    EventBus::get().flush<AnimationFinishedEvent>();
    EventBus::get().sink<AnimationFinishedEvent>().disconnect<&onAnimationFinished>();
    EventBus::get().clear();
    if (g_finished_events != 1 || g_finished_clip != "complete_once")
        return fail(name, "completion did not publish one deferred event with the completed clip name");
    return pass(name);
}

bool testReflectionAndMigration()
{
    const std::string name = "AN-1 migrates authoring data and serializes only SkeletonComponent";
    ReflectionRegistry reflection; registerEngineReflection(reflection);
    if (!reflection.findByName("SkeletonComponent") || reflection.findByName("AnimationComponent"))
        return fail(name, "reflection did not expose only the migrated authoring component");

    entt::registry registry; const entt::entity entity = registry.create();
    auto& binding = registry.emplace<SkeletonComponent>(entity);
    binding.skeleton_path = "assets/fixtures/three_bone.skeleton"; binding.clip_names = {"idle", "run"};
    binding.default_clip = "idle"; binding.playback_speed = 1.25f; binding.loop = false; binding.auto_play = true;
    binding.resources = makeResources(makeRestSkeleton(),
        std::unordered_map<std::string, std::shared_ptr<const AnimationClip>>{{"idle", makeChainClip("idle")}});
    registry.emplace<AnimPlaybackComponent>(entity).current_clip = makeChainClip("runtime");
    registry.emplace<AnimPoseComponent>(entity).bone_matrices.assign(3, glm::mat4(2.0f));
    const nlohmann::json serialized = ReflectionSerializer::serializeEntity(registry, entity, reflection);
    if (!serialized["components"].contains("SkeletonComponent") || serialized["components"].contains("AnimationComponent"))
        return fail(name, "serializer did not write only SkeletonComponent");
    const auto& component = serialized["components"]["SkeletonComponent"];
    for (const char* field : {"resources", "current_clip", "time", "bone_matrices"})
        if (component.contains(field)) return fail(name, "transient field was serialized: " + std::string(field));

    nlohmann::json legacy_level = {{"format", "garden_reflected"}, {"version", 1}, {"entities", nlohmann::json::array({{{"components", {{"AnimationComponent", component}}}}})}};
    entt::registry unmigrated_registry;
    ReflectionSerializer::deserializeLevel(unmigrated_registry, legacy_level, reflection);
    if (!unmigrated_registry.view<SkeletonComponent>().empty())
        return fail(name, "new runtime accepted an unmigrated legacy component record");

    std::string error;
    if (!AnimationComponentMigration::migrateLevel(legacy_level, error) || !error.empty())
        return fail(name, "one-shot legacy conversion failed: " + error);
    const auto& migrated = legacy_level["entities"][0]["components"];
    if (!migrated.contains("SkeletonComponent") || migrated.contains("AnimationComponent"))
        return fail(name, "conversion retained the deprecated component name");

    nlohmann::json legacy_prefab = {{"format", "garden_prefab"}, {"version", 2}, {"nodes", nlohmann::json::array({{{"components", {{"AnimationComponent", component}}}}})}};
    if (!AnimationComponentMigration::migratePrefab(legacy_prefab, error) || !error.empty() ||
        !legacy_prefab["nodes"][0]["components"].contains("SkeletonComponent") ||
        legacy_prefab["nodes"][0]["components"].contains("AnimationComponent"))
        return fail(name, "prefab conversion did not replace the legacy component record");

    entt::registry loaded; ReflectionSerializer::deserializeLevel(loaded, legacy_level, reflection);
    const auto loaded_entity = *loaded.view<SkeletonComponent>().begin();
    const auto& out = loaded.get<SkeletonComponent>(loaded_entity);
    if (out.skeleton_path != binding.skeleton_path || out.clip_names != binding.clip_names ||
        out.default_clip != binding.default_clip || !near(out.playback_speed, binding.playback_speed) ||
        out.loop != binding.loop || out.auto_play != binding.auto_play || out.resources ||
        loaded.any_of<AnimPlaybackComponent, AnimPoseComponent>(loaded_entity))
        return fail(name, "migrated authoring data did not round-trip without runtime state");
    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    ok = testRestPoseAndOverlay() && ok;
    ok = testSystemRestAndInvalidPlayback() && ok;
    ok = testIkOrder() && ok;
    ok = testCompletionEvent() && ok;
    ok = testReflectionAndMigration() && ok;
    return ok ? 0 : 1;
}
