#include "Animation/AnimationBlender.hpp"
#include "Animation/AnimationClip.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Animation/BoneMask.hpp"
#include "Animation/IKSolver.hpp"
#include "Animation/Pose.hpp"
#include "Animation/Skeleton.hpp"
#include "Components/AnimationComponent.hpp"
#include "Components/IKComponent.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

bool nearMatrices(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b,
                  float epsilon = 0.001f)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                if (!near(a[i][column][row], b[i][column][row], epsilon))
                    return false;
            }
        }
    }
    return true;
}

bool differentMatrices(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b,
                       float epsilon = 0.01f)
{
    return !nearMatrices(a, b, epsilon);
}

Bone makeBone(const char* name, int parent, const glm::vec3& local_translation)
{
    Bone bone;
    bone.name = name;
    bone.parent_id = parent;
    bone.local_transform[3] = glm::vec4(local_translation, 1.0f);
    return bone;
}

std::shared_ptr<Skeleton> makeThreeBoneSkeleton()
{
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->addBone(makeBone("root", -1, {0.0f, 0.0f, 0.0f}));
    skeleton->addBone(makeBone("mid", 0, {1.0f, 0.0f, 0.0f}));
    skeleton->addBone(makeBone("end", 1, {1.0f, 0.0f, 0.0f}));
    return skeleton;
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
        channel.position_keys.push_back({0.0f, index == 0 ? glm::vec3(0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)});
        channel.position_keys.push_back({2.0f, index == 0 ? glm::vec3(0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)});
        clip->channels.push_back(std::move(channel));
    }
    return clip;
}

bool testClipSampling()
{
    const std::string name = "AN-0 clip sampling interpolates transforms and defaults missing channels";

    AnimationClip clip;
    clip.duration = 1.0f;
    BoneAnimation channel;
    channel.bone_index = 0;
    channel.position_keys = {{0.0f, {0.0f, 0.0f, 0.0f}}, {1.0f, {8.0f, 2.0f, -4.0f}}};
    channel.scale_keys = {{0.0f, {1.0f, 1.0f, 1.0f}}, {1.0f, {3.0f, 2.0f, 1.0f}}};
    clip.channels.push_back(channel);

    Pose pose;
    clip.samplePose(0.25f, 2, pose);
    if (pose.getBoneCount() != 2 || !nearVec(pose[0].translation, {2.0f, 0.5f, -1.0f}) ||
        !nearVec(pose[0].scale, {1.5f, 1.25f, 1.0f}) || !nearVec(pose[1].translation, {0.0f, 0.0f, 0.0f}) ||
        !nearVec(pose[1].scale, {1.0f, 1.0f, 1.0f}))
        return fail(name, "sampled pose did not interpolate or retain the missing-channel identity");

    std::vector<glm::mat4> matrices;
    clip.sample(0.25f, 2, matrices);
    if (matrices.size() != 2 || !nearVec(glm::vec3(matrices[0][3]), {2.0f, 0.5f, -1.0f}) ||
        !nearVec(glm::vec3(matrices[1][3]), {0.0f, 0.0f, 0.0f}))
        return fail(name, "matrix sampling diverged from decomposed sampling");

    return pass(name);
}

bool testMaskedLayerBlend()
{
    const std::string name = "AN-0 masked override layer changes only weighted bones";

    auto base = makeChainClip("base");
    auto overlay = makeChainClip("overlay");
    for (BoneAnimation& channel : overlay->channels)
    {
        const glm::vec3 translation = channel.bone_index == 0 ? glm::vec3(4.0f, 0.0f, 0.0f)
                                                               : glm::vec3(8.0f, 0.0f, 0.0f);
        channel.position_keys[0].value = translation;
        channel.position_keys[1].value = translation;
    }

    AnimationBlender blender;
    blender.play(base);
    const int overlay_layer = blender.addLayer(AnimationBlendMode::Override);
    AnimationLayer& layer = blender.getLayer(overlay_layer);
    layer.play(overlay);
    layer.mask = BoneMask(3, 0.0f);
    layer.mask.setWeight(1, 1.0f);
    layer.mask.setWeight(2, 1.0f);

    Pose pose;
    blender.updatePose(0.0f, 3, pose);
    if (!nearVec(pose[0].translation, {0.0f, 0.0f, 0.0f}) ||
        !nearVec(pose[1].translation, {8.0f, 0.0f, 0.0f}) ||
        !nearVec(pose[2].translation, {8.0f, 0.0f, 0.0f}))
        return fail(name, "mask weights did not preserve base root and replace selected children");

    return pass(name);
}

bool testMalformedFixtureSafety()
{
    const std::string name = "AN-0 malformed channels are skipped without corrupting the pose";

    AnimationClip clip;
    clip.duration = 1.0f;
    BoneAnimation negative;
    negative.bone_index = -1;
    negative.position_keys.push_back({0.0f, {99.0f, 99.0f, 99.0f}});
    BoneAnimation out_of_range;
    out_of_range.bone_index = 7;
    out_of_range.position_keys.push_back({0.0f, {-99.0f, -99.0f, -99.0f}});
    clip.channels = {negative, out_of_range};

    Pose pose;
    clip.samplePose(0.0f, 2, pose);
    std::vector<glm::mat4> matrices;
    clip.sample(0.0f, 2, matrices);

    if (pose.getBoneCount() != 2 || !nearVec(pose[0].translation, {0.0f, 0.0f, 0.0f}) ||
        !nearVec(pose[1].translation, {0.0f, 0.0f, 0.0f}) || matrices.size() != 2 ||
        !nearVec(glm::vec3(matrices[0][3]), {0.0f, 0.0f, 0.0f}) ||
        !nearVec(glm::vec3(matrices[1][3]), {0.0f, 0.0f, 0.0f}))
        return fail(name, "invalid channel indices wrote outside the requested pose");

    BoneMask mask(2, 0.25f);
    mask.setWeight(-1, 1.0f);
    mask.setWeight(2, 1.0f);
    if (!near(mask.getWeight(0), 0.25f) || !near(mask.getWeight(1), 0.25f))
        return fail(name, "invalid mask indices changed a valid mask entry");

    return pass(name);
}

bool testIkOrder()
{
    const std::string name = "AN-0 ECS animation applies two-bone IK before FABRIK";

    const auto skeleton = makeThreeBoneSkeleton();
    const auto clip = makeChainClip();
    TwoBoneIKConstraint two_bone;
    two_bone.root_bone = 0;
    two_bone.mid_bone = 1;
    two_bone.end_bone = 2;
    two_bone.target = {1.25f, 0.65f, 0.35f};
    two_bone.pole_vector = {0.0f, 0.0f, 1.0f};

    FABRIKConstraint fabrik;
    fabrik.chain = {0, 1, 2};
    fabrik.target = {0.25f, 1.35f, 0.75f};
    fabrik.max_iterations = 16;
    fabrik.tolerance = 0.0001f;

    Pose source_pose;
    clip->samplePose(0.0f, skeleton->getBoneCount(), source_pose);

    Pose expected_pose = source_pose;
    std::vector<glm::mat4> expected_globals;
    skeleton->computeGlobalTransforms(expected_pose, expected_globals);
    TwoBoneIKConstraint::solve(*skeleton, expected_pose, expected_globals, two_bone);
    FABRIKConstraint::solve(*skeleton, expected_pose, expected_globals, fabrik);
    std::vector<glm::mat4> expected_final;
    skeleton->computeFinalMatrices(expected_pose, expected_final);

    Pose reverse_pose = source_pose;
    std::vector<glm::mat4> reverse_globals;
    skeleton->computeGlobalTransforms(reverse_pose, reverse_globals);
    FABRIKConstraint::solve(*skeleton, reverse_pose, reverse_globals, fabrik);
    TwoBoneIKConstraint::solve(*skeleton, reverse_pose, reverse_globals, two_bone);
    std::vector<glm::mat4> reverse_final;
    skeleton->computeFinalMatrices(reverse_pose, reverse_final);
    if (!differentMatrices(expected_final, reverse_final))
        return fail(name, "fixture does not distinguish the required solver ordering");

    EventBus::get().clear();
    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& animation = registry.emplace<AnimationComponent>(entity);
    animation.skeleton = skeleton;
    animation.blender.play(clip);
    auto& ik = registry.emplace<IKComponent>(entity);
    ik.two_bone_constraints.push_back(two_bone);
    ik.fabrik_constraints.push_back(fabrik);

    AnimationSystem::update(registry, 0.0f);
    if (!nearMatrices(animation.bone_matrices, expected_final))
        return fail(name, "system output did not match the two-bone then FABRIK oracle");

    return pass(name);
}

int g_finished_events = 0;
std::string g_finished_clip;
void onAnimationFinished(const AnimationFinishedEvent& event)
{
    ++g_finished_events;
    g_finished_clip = event.clip_name;
}

bool testCompletionEvent()
{
    const std::string name = "AN-0 non-looping completion queues exactly one event";

    EventBus::get().clear();
    EventBus::get().sink<AnimationFinishedEvent>().connect<&onAnimationFinished>();
    g_finished_events = 0;
    g_finished_clip.clear();

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& animation = registry.emplace<AnimationComponent>(entity);
    animation.skeleton = makeThreeBoneSkeleton();
    auto clip = makeChainClip("complete_once");
    clip->duration = 0.25f;
    animation.blender.play(clip);
    animation.blender.setLooping(false);

    AnimationSystem::update(registry, 0.5f);
    EventBus::get().flush<AnimationFinishedEvent>();
    AnimationSystem::update(registry, 0.5f);
    EventBus::get().flush<AnimationFinishedEvent>();
    EventBus::get().sink<AnimationFinishedEvent>().disconnect<&onAnimationFinished>();
    EventBus::get().clear();

    if (g_finished_events != 1 || g_finished_clip != "complete_once")
        return fail(name, "completion did not publish one deferred event with the completed clip name");

    return pass(name);
}

bool testReflectionBoundary()
{
    const std::string name = "AN-0 animation reflection keeps runtime state transient";

    ReflectionRegistry reflection;
    registerEngineReflection(reflection);
    if (!reflection.findByName("AnimationComponent") || !reflection.findByName("IKComponent"))
        return fail(name, "engine reflection did not register animation authoring components");

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& animation = registry.emplace<AnimationComponent>(entity);
    animation.skeleton_path = "assets/fixtures/three_bone.skeleton";
    animation.clip_names = {"idle", "run"};
    animation.default_clip = "idle";
    animation.playback_speed = 1.25f;
    animation.loop = false;
    animation.auto_play = true;
    animation.skeleton = makeThreeBoneSkeleton();
    animation.addClip("idle", makeChainClip("idle"));
    animation.playClip("idle");
    animation.bone_matrices.assign(3, glm::mat4(2.0f));
    registry.emplace<IKComponent>(entity).enabled = false;

    const nlohmann::json serialized = ReflectionSerializer::serializeEntity(registry, entity, reflection);
    const nlohmann::json& component = serialized["components"]["AnimationComponent"];
    for (const char* field : {"skeleton", "clips", "blender", "bone_matrices"})
    {
        if (component.contains(field))
            return fail(name, "transient field was serialized: " + std::string(field));
    }

    entt::registry loaded;
    const entt::entity loaded_entity = loaded.create();
    ReflectionSerializer::deserializeEntity(loaded, loaded_entity, serialized, reflection);
    const auto& out = loaded.get<AnimationComponent>(loaded_entity);
    if (out.skeleton_path != animation.skeleton_path || out.clip_names != animation.clip_names ||
        out.default_clip != animation.default_clip || !near(out.playback_speed, animation.playback_speed) ||
        out.loop != animation.loop || out.auto_play != animation.auto_play || out.skeleton ||
        !out.clips.empty() || !out.bone_matrices.empty() || out.blender.isPlaying())
        return fail(name, "authoring fields did not round-trip cleanly across the runtime boundary");

    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    ok = testClipSampling() && ok;
    ok = testMaskedLayerBlend() && ok;
    ok = testMalformedFixtureSafety() && ok;
    ok = testIkOrder() && ok;
    ok = testCompletionEvent() && ok;
    ok = testReflectionBoundary() && ok;
    return ok ? 0 : 1;
}
