#define GLM_ENABLE_EXPERIMENTAL

#include "Animation/AnimationGraph.hpp"
#include "Animation/AnimationGraphSystem.hpp"
#include "Assets/AssetManager.hpp"
#include "Animation/CompiledAnimationSerializer.hpp"
#include "Animation/GltfAnimationLoader.hpp"
#include "Animation/AnimationResourceValidation.hpp"
#include "Animation/AnimationResourceService.hpp"
#include "Animation/AnimationResourceBindingSystem.hpp"
#include "Animation/AnimationClip.hpp"
#include "Animation/AnimationSampling.hpp"
#include "Animation/AnimationSystem.hpp"
#include "Animation/IKSolver.hpp"
#include "Animation/Pose.hpp"
#include "Animation/Skeleton.hpp"
#include "Components/AnimPlaybackComponent.hpp"
#include "Components/AnimGraphComponent.hpp"
#include "Components/AnimGraphStateComponent.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Components/AnimResourceBindingComponent.hpp"
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
#include <tiny_gltf.h>

#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
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
    std::vector<Bone> bones;
    bones.push_back(makeBone("root", -1, {3.0f, -2.0f, 1.0f}, glm::angleAxis(0.4f, glm::vec3(0, 0, 1)), {1.5f, 1.0f, 0.75f}));
    bones.push_back(makeBone("mid", 0, {1.0f, 2.0f, 0.5f}, glm::angleAxis(-0.2f, glm::vec3(0, 1, 0)), {0.8f, 1.2f, 1.0f}));
    bones.push_back(makeBone("end", 1, {-0.5f, 0.25f, 2.0f}, glm::angleAxis(0.3f, glm::vec3(1, 0, 0)), {1.0f, 0.9f, 1.1f}));
    for (int index = 0; index < static_cast<int>(bones.size()); ++index) bones[index].id = index;
    AnimationResourceError error;
    auto skeleton = AnimationResourceValidation::makeSkeleton(std::move(bones), error);
    return skeleton;
}

std::shared_ptr<const AnimationResourceSet> makeResources(
    std::shared_ptr<const Skeleton> skeleton,
    std::vector<std::shared_ptr<const AnimationClip>> clips = {})
{
    AnimationResourceError error;
    return AnimationResourceValidation::makeResourceSet(
        std::move(skeleton), std::move(clips), "animation-tests", error);
}

std::shared_ptr<const AnimationClip> makeChainClip(const char* name = "chain", float duration = 2.0f)
{
    AnimationClipDefinition clip;
    clip.name = name;
    clip.duration = duration;
    for (int index = 0; index < 3; ++index)
    {
        BoneAnimation channel;
        channel.bone_index = index;
        channel.position_keys = {{0.0f, {0.0f, 0.0f, 0.0f}}, {duration, {0.0f, 0.0f, 0.0f}}};
        channel.rotation_keys = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
        channel.scale_keys = {{0.0f, {1.0f, 1.0f, 1.0f}}};
        clip.channels.push_back(std::move(channel));
    }
    AnimationResourceError error;
    return AnimationResourceValidation::makeClip(std::move(clip), 3, error);
}

std::shared_ptr<const AnimationClip> makeClip(AnimationClipDefinition clip, int bone_count = 3)
{
    AnimationResourceError error;
    return AnimationResourceValidation::makeClip(std::move(clip), bone_count, error);
}

namespace fs = std::filesystem;

bool readBinary(const fs::path& path, std::vector<unsigned char>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(length));
    return bytes.empty() || static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), length));
}

bool writeBinary(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

void writeU32(std::vector<unsigned char>& bytes, size_t offset, uint32_t value)
{
    for (size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<unsigned char>(value >> (index * 8));
}

uint32_t readU32(const std::vector<unsigned char>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void writeU64(std::vector<unsigned char>& bytes, size_t offset, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<unsigned char>(value >> (index * 8));
}

uint32_t crc32(const std::vector<unsigned char>& bytes, size_t offset)
{
    uint32_t crc = 0xffffffffu;
    for (size_t index = offset; index < bytes.size(); ++index)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool rejectsCanm(const fs::path& path, const char* expected_code)
{
    CompiledAnimationData decoded;
    AnimationResourceError error;
    return !CompiledAnimationSerializer::load(decoded, path.string(), &error) &&
        error.code == expected_code && decoded.skins.empty();
}

bool testCanmSerialization()
{
    const std::string name = "AN-2 CANM packages decode deterministically and reject corrupt bytes";
    const fs::path root = fs::temp_directory_path() / "garden_an2_canm_tests";
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::create_directories(root, cleanup_error);
    if (cleanup_error) return fail(name, "could not create temporary CANM directory");
    const auto cleanup = [&]() { std::error_code ignored; fs::remove_all(root, ignored); };

    const auto skeleton = makeRestSkeleton();
    CompiledAnimationData source;
    source.header.source_hash = 0x1122334455667788ull;
    source.header.cook_fingerprint = 0x8877665544332211ull;
    source.skins = {
        {3, skeleton, {makeChainClip("run")}, "fixture/skin-three"},
        {1, skeleton, {makeChainClip("idle")}, "fixture/skin-one"},
    };
    const fs::path package = root / "fixture.canim";
    const fs::path package_second = root / "fixture-second.canim";
    AnimationResourceError error;
    if (!CompiledAnimationSerializer::save(source, package.string(), &error) || !error.code.empty() ||
        !CompiledAnimationSerializer::save(source, package_second.string(), &error))
    {
        cleanup();
        return fail(name, "valid immutable resources did not serialize: " + error.code);
    }
    std::vector<unsigned char> valid;
    std::vector<unsigned char> repeated;
    if (!readBinary(package, valid) || !readBinary(package_second, repeated) || valid != repeated ||
        valid.size() <= CANM_HEADER_BYTES)
    {
        cleanup();
        return fail(name, "CANM output was not deterministic");
    }
    CompiledAnimationData loaded;
    if (!CompiledAnimationSerializer::load(loaded, package.string(), &error) || !error.code.empty() ||
        loaded.header.source_hash != source.header.source_hash ||
        loaded.header.cook_fingerprint != source.header.cook_fingerprint || loaded.skins.size() != 2 ||
        loaded.skins[0].skin_index != 1 || loaded.skins[1].skin_index != 3 || !loaded.skins[0].resource ||
        !loaded.skins[0].resource->findClip("idle") || !loaded.skins[1].resource->findClip("run"))
    {
        cleanup();
        return fail(name, "valid CANM package did not publish canonical immutable resources");
    }

    const auto writeAndReject = [&](const char* filename, std::vector<unsigned char> bytes,
                                    const char* expected_code) {
        const fs::path corrupted = root / filename;
        return writeBinary(corrupted, bytes) && rejectsCanm(corrupted, expected_code);
    };
    std::vector<unsigned char> bad = valid;
    bad[0] ^= 0x01;
    if (!writeAndReject("bad_magic.canim", bad, "CANM_MAGIC"))
    {
        cleanup(); return fail(name, "bad CANM magic was accepted or unnamed");
    }
    bad = valid;
    writeU32(bad, 4, CANM_VERSION + 1);
    if (!writeAndReject("bad_version.canim", bad, "CANM_VERSION"))
    {
        cleanup(); return fail(name, "bad CANM version was accepted or unnamed");
    }
    bad = valid;
    writeU32(bad, 8, 0x10);
    if (!writeAndReject("bad_flags.canim", bad, "CANM_FLAGS"))
    {
        cleanup(); return fail(name, "unknown CANM flags were accepted or unnamed");
    }
    bad = valid;
    writeU32(bad, 12, 1);
    if (!writeAndReject("bad_reserved.canim", bad, "CANM_RESERVED"))
    {
        cleanup(); return fail(name, "non-zero CANM reserved header was accepted");
    }
    bad = valid;
    bad.push_back(0);
    if (!writeAndReject("trailing_file.canim", bad, "CANM_TRAILING_BYTES"))
    {
        cleanup(); return fail(name, "trailing CANM file bytes were accepted or unnamed");
    }
    bad = valid;
    bad.push_back(0);
    writeU64(bad, 32, static_cast<uint64_t>(bad.size() - CANM_HEADER_BYTES));
    writeU32(bad, 44, crc32(bad, CANM_HEADER_BYTES));
    if (!writeAndReject("trailing_payload.canim", bad, "CANM_TRAILING_BYTES"))
    {
        cleanup(); return fail(name, "declared CANM payload trailing bytes were accepted");
    }
    bad = valid;
    bad[CANM_HEADER_BYTES] ^= 0x80;
    if (!writeAndReject("bad_crc.canim", bad, "CANM_CRC"))
    {
        cleanup(); return fail(name, "corrupt CANM payload did not fail checksum validation");
    }
    bad = valid;
    bad[CANM_HEADER_BYTES + 8] = 0xff; // first source-signature byte
    writeU32(bad, 44, crc32(bad, CANM_HEADER_BYTES));
    if (!writeAndReject("bad_utf8.canim", bad, "CANM_UTF8"))
    {
        cleanup(); return fail(name, "invalid CANM UTF-8 was accepted or unnamed");
    }
    bad = valid;
    // First skin: index + signature length + signature + bone/clip counts, then bone ID.
    const uint32_t signature_length = static_cast<uint32_t>(source.skins[1].source_signature.size());
    const size_t first_bone_id = CANM_HEADER_BYTES + 4 + 4 + signature_length + 4 + 4;
    writeU32(bad, first_bone_id, 99);
    writeU32(bad, 44, crc32(bad, CANM_HEADER_BYTES));
    if (!writeAndReject("bad_bone.canim", bad, "ANIM_BONE_PARENT_ORDER"))
    {
        cleanup(); return fail(name, "invalid CANM bone table bypassed immutable skeleton validation");
    }
    bad = valid;
    size_t first_channel = CANM_HEADER_BYTES + 4;
    first_channel += 4 + readU32(bad, first_channel); // source signature
    first_channel += 8; // bone and clip counts
    const uint32_t bones = readU32(bad, CANM_HEADER_BYTES + 4 + 4 + signature_length);
    for (uint32_t index = 0; index < bones; ++index)
    {
        first_channel += 8; // ID and parent
        first_channel += 4 + readU32(bad, first_channel); // name
        first_channel += 16 * sizeof(float) * 2; // inverse bind and rest matrices
    }
    first_channel += 4 + readU32(bad, first_channel); // clip name
    first_channel += 4 + 4 + 4; // timing and channel count
    writeU32(bad, first_channel + 4, 0x80);
    writeU32(bad, 44, crc32(bad, CANM_HEADER_BYTES));
    if (!writeAndReject("bad_channel_flags.canim", bad, "CANM_CHANNEL_FLAGS"))
    {
        cleanup(); return fail(name, "unknown CANM channel flags were accepted or unnamed");
    }
    bad = valid;
    bad.resize(CANM_HEADER_BYTES + 7);
    if (!writeAndReject("truncated_payload.canim", bad, "CANM_PAYLOAD_SIZE"))
    {
        cleanup(); return fail(name, "truncated CANM payload was accepted or unnamed");
    }
    cleanup();
    return pass(name);
}

bool testResourceServiceAndBinding()
{
    const std::string name = "AN-2 service deduplicates immutable CANM and binding retains a failed replacement";
    const fs::path root = fs::temp_directory_path() / "garden_an2_resource_service";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) return fail(name, "could not create service fixture directory");
    const auto cleanup = [&]() {
        AnimationResourceService::get().shutdown();
        Assets::AssetManager::get().setAssetRoot({});
        std::error_code ignored;
        fs::remove_all(root, ignored);
    };

    CompiledAnimationData source;
    source.skins = {{0, makeRestSkeleton(), {makeChainClip("idle")}, "service-fixture"}};
    AnimationResourceError error;
    if (!CompiledAnimationSerializer::save(source, (root / "hero.canim").string(), &error))
    {
        cleanup();
        return fail(name, "could not write valid CANM fixture: " + error.code);
    }

    Assets::AssetManager::get().setAssetRoot(root.string());
    const AnimationResourceKey valid{"hero.canim", 0};
    const auto first = AnimationResourceService::get().request(valid);
    const auto duplicate = AnimationResourceService::get().request({"./hero.canim", 0});
    std::shared_ptr<const AnimationResourceSet> ready;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!ready && std::chrono::steady_clock::now() < deadline)
    {
        AnimationResourceService::get().update();
        ready = AnimationResourceService::get().tryGetReady(first);
        std::this_thread::yield();
    }
    if (!ready || AnimationResourceService::get().tryGetReady(duplicate) != ready)
    {
        cleanup();
        return fail(name, "canonical requests did not share one published immutable resource");
    }

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& authored = registry.emplace<SkeletonComponent>(entity);
    authored.animation_resource_path = "hero.canim";
    authored.default_clip = "idle";
    authored.auto_play = true;
    AnimationResourceBindingSystem::update(registry);
    auto& binding = registry.get<AnimResourceBindingComponent>(entity);
    if (binding.active != ready)
    {
        cleanup();
        return fail(name, "binding did not consume the service safe-boundary publication");
    }
    AnimationSystem::update(registry, 0.0f, false);
    auto& playback = registry.get<AnimPlaybackComponent>(entity);
    if (!playback.current_clip || !playback.playing)
    {
        cleanup();
        return fail(name, "first valid binding did not seed its default clip");
    }

    authored.animation_resource_path = "missing.txt";
    AnimationResourceBindingSystem::update(registry);
    if (binding.active != ready || binding.state != AnimationResourceState::Failed ||
        !AnimationResourceService::get().getError(binding.ticket))
    {
        cleanup();
        return fail(name, "failed replacement cleared the last valid resource");
    }
    AnimationSystem::update(registry, 0.0f, false);
    if (!registry.get<AnimPoseComponent>(entity).pose_valid)
    {
        cleanup();
        return fail(name, "failed replacement did not retain the valid rest-pose boundary");
    }
    cleanup();
    return pass(name);
}
bool testImmutableResourceValidation()
{
    const std::string name = "AN-2 validates immutable skeleton and clip resources before publication";
    const auto skeleton = makeRestSkeleton();
    if (!skeleton || skeleton->getBoneCount() != 3 || skeleton->getBoneByName("mid")->id != 1)
        return fail(name, "valid ordered skeleton was not published");

    std::vector<Bone> invalid = {makeBone("root", -1, {}), makeBone("child", 1, {})};
    invalid[0].id = 0; invalid[1].id = 1;
    AnimationResourceError error;
    if (AnimationResourceValidation::makeSkeleton(invalid, error) || error.code != "ANIM_BONE_PARENT_ORDER")
        return fail(name, "parent-after-child skeleton was accepted");

    invalid = {makeBone("root", -1, {}), makeBone("root", 0, {})};
    invalid[0].id = 0; invalid[1].id = 1;
    if (AnimationResourceValidation::makeSkeleton(invalid, error) || error.code != "ANIM_BONE_NAME")
        return fail(name, "duplicate bone name was accepted");

    AnimationClipDefinition duplicate;
    duplicate.name = "duplicate"; duplicate.duration = 1.0f;
    for (int index = 0; index < 2; ++index)
    {
        BoneAnimation channel; channel.bone_index = 0;
        channel.position_keys = {{0.0f, {0.0f, 0.0f, 0.0f}}};
        duplicate.channels.push_back(std::move(channel));
    }
    if (AnimationResourceValidation::makeClip(duplicate, skeleton->getBoneCount(), error) ||
        error.code != "ANIM_DUPLICATE_CHANNEL")
        return fail(name, "duplicate clip channel was accepted");

    AnimationClipDefinition nonfinite;
    nonfinite.name = "nonfinite"; nonfinite.duration = 0.0f;
    BoneAnimation channel; channel.bone_index = 0;
    channel.rotation_keys = {{0.0f, glm::quat(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f)}};
    nonfinite.channels.push_back(std::move(channel));
    if (AnimationResourceValidation::makeClip(nonfinite, skeleton->getBoneCount(), error) ||
        error.code != "ANIM_INVALID_KEY")
        return fail(name, "non-finite rotation key was accepted");

    const auto idle = makeChainClip("idle");
    if (!makeResources(skeleton, {idle}) || makeResources(skeleton, {idle, idle}))
        return fail(name, "immutable set did not enforce unique named clips");
    return pass(name);
}
template<typename T>
void appendBytes(std::vector<unsigned char>& bytes, const std::vector<T>& values)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + values.size() * sizeof(T));
    std::memcpy(bytes.data() + offset, values.data(), values.size() * sizeof(T));
}

int addAccessor(tinygltf::Model& model, size_t offset, size_t byte_count, size_t count, int type)
{
    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = offset;
    view.byteLength = byte_count;
    model.bufferViews.push_back(view);
    tinygltf::Accessor accessor;
    accessor.bufferView = static_cast<int>(model.bufferViews.size() - 1);
    accessor.byteOffset = 0;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = type;
    accessor.count = count;
    model.accessors.push_back(accessor);
    return static_cast<int>(model.accessors.size() - 1);
}

tinygltf::Model makeCheckedGltf()
{
    tinygltf::Model model;
    tinygltf::Node root;
    root.name = "root";
    model.nodes.push_back(root);
    tinygltf::Skin skin;
    skin.name = "test-skin";
    skin.joints = {0};
    model.skins.push_back(skin);

    tinygltf::Buffer buffer;
    const std::vector<float> times = {0.0f, 1.0f};
    const std::vector<float> translations = {0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f};
    appendBytes(buffer.data, times);
    const size_t translation_offset = buffer.data.size();
    appendBytes(buffer.data, translations);
    model.buffers.push_back(std::move(buffer));
    const int time_accessor = addAccessor(model, 0, times.size() * sizeof(float), times.size(), TINYGLTF_TYPE_SCALAR);
    const int translation_accessor = addAccessor(model, translation_offset,
        translations.size() * sizeof(float), times.size(), TINYGLTF_TYPE_VEC3);

    tinygltf::Animation animation;
    animation.name = "move";
    tinygltf::AnimationSampler sampler;
    sampler.input = time_accessor;
    sampler.output = translation_accessor;
    sampler.interpolation = "LINEAR";
    animation.samplers.push_back(sampler);
    tinygltf::AnimationChannel channel;
    channel.sampler = 0;
    channel.target_node = 0;
    channel.target_path = "translation";
    animation.channels.push_back(channel);
    model.animations.push_back(std::move(animation));
    return model;
}

bool rejectsGltf(tinygltf::Model model, const char* expected_code)
{
    GltfAnimationData data;
    AnimationResourceError error;
    if (GltfAnimationLoader::load(model, data, &error) || error.code != expected_code ||
        data.skeleton || !data.clips.empty())
        return false;
    return true;
}

bool testCheckedGltfAnimationSource()
{
    const std::string name = "AN-2 glTF animation source rejects malformed accessors and structural input";
    tinygltf::Model valid = makeCheckedGltf();
    GltfAnimationData data;
    AnimationResourceError error;
    if (!GltfAnimationLoader::load(valid, data, &error) || !error.code.empty() || !data.skeleton ||
        data.clips.size() != 1 || data.clips[0]->name() != "move" || !data.has_animation)
        return fail(name, "valid checked glTF source did not publish one immutable resource");

    tinygltf::Model bad = valid;
    bad.accessors[0].bufferView = 99;
    if (!rejectsGltf(std::move(bad), "ANIM_ACCESSOR_VIEW"))
        return fail(name, "invalid accessor view was accepted or unnamed");

    bad = valid;
    bad.bufferViews[0].byteStride = 8;
    if (!rejectsGltf(std::move(bad), "ANIM_ACCESSOR_LAYOUT"))
        return fail(name, "interleaved animation accessor was accepted");

    bad = valid;
    bad.accessors[0].byteOffset = 4096;
    if (!rejectsGltf(std::move(bad), "ANIM_ACCESSOR_RANGE"))
        return fail(name, "out-of-range animation accessor was accepted");

    bad = valid;
    bad.accessors[0].sparse.isSparse = true;
    if (!rejectsGltf(std::move(bad), "ANIM_ACCESSOR_SPARSE"))
        return fail(name, "sparse animation accessor was accepted");

    bad = valid;
    bad.animations[0].channels[0].sampler = -1;
    if (!rejectsGltf(std::move(bad), "ANIM_SAMPLER_INDEX"))
        return fail(name, "invalid animation sampler index was accepted");

    bad = valid;
    bad.animations[0].samplers[0].output = 0;
    if (!rejectsGltf(std::move(bad), "ANIM_ACCESSOR_TYPE"))
        return fail(name, "wrong output accessor shape was accepted");

    bad = valid;
    bad.accessors[1].count = 1;
    if (!rejectsGltf(std::move(bad), "ANIM_SAMPLER_COUNT"))
        return fail(name, "mismatched sampler key counts were accepted");

    bad = valid;
    bad.animations[0].samplers[0].interpolation = "CUBICSPLINE";
    if (!rejectsGltf(std::move(bad), "ANIM_UNSUPPORTED_INTERPOLATION"))
        return fail(name, "unsupported interpolation was accepted");

    bad = valid;
    const float duplicate_time = 0.0f;
    std::memcpy(bad.buffers[0].data.data() + sizeof(float), &duplicate_time, sizeof(float));
    if (!rejectsGltf(std::move(bad), "ANIM_INVALID_KEY"))
        return fail(name, "non-increasing key times were accepted");

    bad = valid;
    bad.skins[0].joints = {0, 0};
    if (!rejectsGltf(std::move(bad), "ANIM_BONE_DUPLICATE"))
        return fail(name, "duplicate skin joint was accepted");

    bad = valid;
    bad.nodes[0].children.push_back(9);
    if (!rejectsGltf(std::move(bad), "ANIM_BONE_NODE"))
        return fail(name, "invalid skeleton child node was accepted");

    bad = valid;
    tinygltf::Node intermediary;
    intermediary.name = "intermediary";
    intermediary.children = {0};
    bad.nodes.push_back(intermediary);
    if (!rejectsGltf(std::move(bad), "ANIM_BONE_HIERARCHY"))
        return fail(name, "non-joint immediate parent was accepted");

    bad = valid;
    tinygltf::Node second_joint;
    second_joint.name = "second";
    bad.nodes.push_back(second_joint);
    bad.skins[0].joints = {0, 1};
    const size_t bind_offset = bad.buffers[0].data.size();
    appendBytes(bad.buffers[0].data, std::vector<float>(16, 1.0f));
    bad.skins[0].inverseBindMatrices = addAccessor(bad, bind_offset, 16 * sizeof(float), 1, TINYGLTF_TYPE_MAT4);
    if (!rejectsGltf(std::move(bad), "ANIM_BIND_COUNT"))
        return fail(name, "partial inverse bind array was accepted");

    bad = valid;
    bad.animations[0].name.clear();
    if (!rejectsGltf(std::move(bad), "ANIM_CLIP_NAME"))
        return fail(name, "unnamed clip was accepted");

    bad = valid;
    bad.animations[0].channels.push_back(bad.animations[0].channels[0]);
    if (!rejectsGltf(std::move(bad), "ANIM_DUPLICATE_CHANNEL"))
        return fail(name, "duplicate property channel was accepted");
    return pass(name);
}

bool testRestPoseAndOverlay()
{
    const std::string name = "AN-1 pose sampling preserves skeleton rest until authored keys override it";
    const auto skeleton = makeRestSkeleton();
    Pose rest;
    AnimationSampling::makeRestPose(*skeleton, rest);

    AnimationClipDefinition empty;
    empty.name = "empty";
    AnimationResourceError error;
    if (AnimationResourceValidation::makeClip(empty, skeleton->getBoneCount(), error) || error.code != "ANIM_EMPTY_CLIP")
        return fail(name, "empty clip bypassed immutable validation");

    AnimationClipDefinition partial;
    partial.name = "partial";
    partial.duration = 0.0f;
    BoneAnimation channel;
    channel.bone_index = 1;
    channel.position_keys = {{0.0f, {9.0f, 8.0f, 7.0f}}};
    partial.channels.push_back(channel);
    const auto partial_clip = makeClip(std::move(partial));
    Pose partial_result = rest;
    AnimationSampling::overlayClip(*partial_clip, 0.0f, partial_result);
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

    AnimationClipDefinition explicit_identity;
    explicit_identity.name = "identity";
    explicit_identity.duration = 0.0f;
    BoneAnimation identity;
    identity.bone_index = 0;
    identity.position_keys = {{0.0f, glm::vec3(0.0f)}};
    identity.rotation_keys = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
    identity.scale_keys = {{0.0f, glm::vec3(1.0f)}};
    explicit_identity.channels.push_back(identity);
    const auto identity_clip = makeClip(std::move(explicit_identity));
    Pose identity_result = rest;
    AnimationSampling::overlayClip(*identity_clip, 0.0f, identity_result);
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
    registry.emplace<AnimResourceBindingComponent>(entity).active = makeResources(skeleton);
    binding.auto_play = true;
    binding.default_clip = "missing";
    AnimationSystem::update(registry, 0.0f, false);
    if (!registry.all_of<AnimPlaybackComponent, AnimPoseComponent>(entity))
        return fail(name, "runtime components were not initialized for a serialized skeleton binding");
    auto& playback = registry.get<AnimPlaybackComponent>(entity);
    auto& pose = registry.get<AnimPoseComponent>(entity);
    if (!pose.pose_valid || !nearPose(pose.local_pose, expected_rest) || !pose.bone_matrices.empty())
        return fail(name, "stopped binding did not evaluate rest or allocated a headless palette");

    playback.current_clip.reset();
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
    registry.emplace<AnimResourceBindingComponent>(entity).active = makeResources(skeleton);
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
    auto& binding = registry.emplace<SkeletonComponent>(entity);
    registry.emplace<AnimResourceBindingComponent>(entity).active = makeResources(makeRestSkeleton());
    auto& playback = registry.emplace<AnimPlaybackComponent>(entity);
    playback.initialized = true;
    auto completion_clip = makeChainClip("complete_once", 0.25f);
    playback.current_clip = completion_clip;
    playback.playing = true; playback.looping = false;
    AnimationSystem::update(registry, 0.5f);
    EventBus::get().flush<AnimationFinishedEvent>();
    Pose completed_rest;
    AnimationSampling::makeRestPose(*registry.get<AnimResourceBindingComponent>(entity).active->skeleton(), completed_rest);
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

bool testReflectionHardCutover()
{
    const std::string name = "animation authoring serializes only the current hard-cutover schema";
    ReflectionRegistry reflection;
    registerEngineReflection(reflection);
    if (!reflection.findByName("SkeletonComponent") || reflection.findByName("AnimationComponent"))
        return fail(name, "reflection did not expose only the current authoring component");

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& binding = registry.emplace<SkeletonComponent>(entity);
    binding.animation_resource_path = "assets/fixtures/three_bone.canim";
    binding.animation_skin_index = 0;
    binding.default_clip = "idle";
    binding.playback_speed = 1.25f;
    binding.loop = false;
    binding.auto_play = true;
    registry.emplace<AnimPlaybackComponent>(entity).current_clip = makeChainClip("runtime");
    registry.emplace<AnimPoseComponent>(entity).bone_matrices.assign(3, glm::mat4(2.0f));

    const nlohmann::json serialized = ReflectionSerializer::serializeEntity(registry, entity, reflection);
    if (!serialized["components"].contains("SkeletonComponent") || serialized["components"].contains("AnimationComponent"))
        return fail(name, "serializer did not write only SkeletonComponent");
    const auto& component = serialized["components"]["SkeletonComponent"];
    for (const char* field : {"resources", "current_clip", "time", "bone_matrices"})
        if (component.contains(field))
            return fail(name, "transient field was serialized: " + std::string(field));

    nlohmann::json current_record = {{"components", nlohmann::json::object()}};
    current_record["components"]["SkeletonComponent"] = component;
    const nlohmann::json current_level = {
        {"format", "garden_reflected"},
        {"version", 1},
        {"entities", nlohmann::json::array({current_record})}
    };
    entt::registry loaded;
    ReflectionSerializer::deserializeLevel(loaded, current_level, reflection);
    if (loaded.view<SkeletonComponent>().empty())
        return fail(name, "current SkeletonComponent payload was rejected");
    const auto loaded_entity = *loaded.view<SkeletonComponent>().begin();
    const auto& out = loaded.get<SkeletonComponent>(loaded_entity);
    if (out.animation_resource_path != binding.animation_resource_path ||
        out.animation_skin_index != binding.animation_skin_index ||
        out.default_clip != binding.default_clip || !near(out.playback_speed, binding.playback_speed) ||
        out.loop != binding.loop || out.auto_play != binding.auto_play ||
        loaded.any_of<AnimPlaybackComponent, AnimPoseComponent>(loaded_entity))
        return fail(name, "current authoring data did not round-trip without runtime state");

    nlohmann::json old_component_record = {{"components", nlohmann::json::object()}};
    old_component_record["components"]["AnimationComponent"] = component;
    const nlohmann::json old_component_level = {
        {"format", "garden_reflected"},
        {"version", 1},
        {"entities", nlohmann::json::array({old_component_record})}
    };
    entt::registry old_component_registry;
    ReflectionSerializer::deserializeLevel(old_component_registry, old_component_level, reflection);
    if (!old_component_registry.view<SkeletonComponent>().empty())
        return fail(name, "legacy AnimationComponent record was accepted");

    nlohmann::json legacy_fields = component;
    legacy_fields["skeleton_path"] = "assets/legacy.canim";
    legacy_fields["clip_names"] = nlohmann::json::array({"idle"});
    nlohmann::json legacy_fields_entity = {{"components", nlohmann::json::object()}};
    legacy_fields_entity["components"]["SkeletonComponent"] = std::move(legacy_fields);
    entt::registry legacy_fields_registry;
    ReflectionSerializer::deserializeEntity(legacy_fields_registry, legacy_fields_registry.create(),
        legacy_fields_entity, reflection);
    if (!legacy_fields_registry.view<SkeletonComponent>().empty())
        return fail(name, "legacy SkeletonComponent fields were accepted");
    return pass(name);
}
std::shared_ptr<const AnimationClip> makeGraphClip()
{
    AnimationClipDefinition definition;
    definition.name = "graph_move";
    definition.duration = 1.0f;
    BoneAnimation root;
    root.bone_index = 0;
    root.position_keys = {{0.0f, {0.0f, 0.0f, 0.0f}}, {1.0f, {8.0f, 0.0f, 0.0f}}};
    definition.channels.push_back(std::move(root));
    return makeClip(std::move(definition));
}

std::shared_ptr<const AnimGraph> makeGraphFoundation(
    const std::shared_ptr<const AnimationResourceSet>& resources)
{
    AnimGraphDefinition definition;
    definition.resource_signature = resources->sourceSignature();
    definition.parameters = {
        {"grounded", AnimGraphParameterType::Bool, true},
        {"mode", AnimGraphParameterType::Int, int32_t{2}},
        {"speed", AnimGraphParameterType::Float, 3.5f},
        {"target", AnimGraphParameterType::Vec3, glm::vec3(1.0f, 2.0f, 3.0f)},
    };
    definition.states = {{"move", 0, AnimGraphPoseSource::Clip, "graph_move", 1.0f, true}};
    definition.machines = {{"locomotion", 0}};
    AnimGraphValidationReport report;
    return AnimGraphValidation::makeGraph(std::move(definition), resources, report);
}

bool testGraphFoundationValidation()
{
    const std::string name = "AN-3 graph resources validate atomically and stay owner-bound";
    const auto resources = makeResources(makeRestSkeleton(), {makeGraphClip()});
    if (!resources) return fail(name, "could not build immutable animation resources");

    AnimGraphDefinition valid;
    valid.resource_signature = resources->sourceSignature();
    valid.parameters = {{"ready", AnimGraphParameterType::Bool, false}};
    valid.states = {{"idle", 0, AnimGraphPoseSource::RestPose, "", 1.0f, true},
                    {"move", 0, AnimGraphPoseSource::Clip, "graph_move", 1.0f, true}};
    valid.machines = {{"base", 0}};
    AnimGraphValidationReport report;
    const auto graph = AnimGraphValidation::makeGraph(valid, resources, report);
    if (!graph || !report.valid() || graph->states().size() != 2 ||
        graph->resourceSet() != resources)
        return fail(name, "valid definition did not publish one immutable owner-bound graph");

    AnimGraphDefinition invalid = valid;
    invalid.resource_signature = "other-resource";
    invalid.parameters.push_back({"ready", AnimGraphParameterType::Float, false});
    invalid.parameters.push_back({"bad-number", AnimGraphParameterType::Float,
                                  std::numeric_limits<float>::infinity()});
    invalid.states[0].clip_name = "not-allowed";
    invalid.states[1].clip_name = "missing";
    invalid.states[1].speed = std::numeric_limits<float>::quiet_NaN();
    invalid.states.push_back({"outside", 9, AnimGraphPoseSource::RestPose, "", 1.0f, true});
    invalid.machines.push_back({"base", 0});
    const auto rejected = AnimGraphValidation::makeGraph(std::move(invalid), resources, report);
    if (rejected || report.valid() || report.errors.size() < 7)
        return fail(name, "invalid graph was published or did not report all independent defects");
    return pass(name);
}

bool testGraphFixedTickAndEvaluation()
{
    const std::string name = "AN-3 graph clocks advance on fixed ticks and evaluation is read-only";
    const auto resources = makeResources(makeRestSkeleton(), {makeGraphClip()});
    const auto graph = makeGraphFoundation(resources);
    if (!graph) return fail(name, "could not create validated graph");

    entt::registry registry;
    const entt::entity entity = registry.create();
    registry.emplace<AnimGraphComponent>(entity).graph = graph;
    registry.emplace<AnimGraphStateComponent>(entity);
    AnimGraphSystem::advance(registry, 0.25f);
    auto& state = registry.get<AnimGraphStateComponent>(entity);
    if (!state.initialized || state.machines.size() != 1 || state.machines[0].active_state != 0 ||
        !near(state.machines[0].state_time, 0.25f) || state.parameters.size() != 4 ||
        !std::get<bool>(state.parameters[0]) || std::get<int32_t>(state.parameters[1]) != 2 ||
        !near(std::get<float>(state.parameters[2]), 3.5f) ||
        !nearVec(std::get<glm::vec3>(state.parameters[3]), {1.0f, 2.0f, 3.0f}))
        return fail(name, "fixed advance did not initialize ECS defaults and state clock");
    if (AnimGraphSystem::activeState(registry, entity, "locomotion") != 0 ||
        !AnimGraphSystem::setBool(registry, entity, "grounded", false) ||
        !AnimGraphSystem::setInt(registry, entity, "mode", 7) ||
        !AnimGraphSystem::setFloat(registry, entity, "speed", 1.5f) ||
        !AnimGraphSystem::setVec3(registry, entity, "target", {4.0f, 5.0f, 6.0f}) ||
        AnimGraphSystem::setFloat(registry, entity, "mode", 1.0f) ||
        AnimGraphSystem::setFloat(registry, entity, "missing", 1.0f))
        return fail(name, "typed parameter writes did not retain their strict graph schema");

    const float before_evaluate = state.machines[0].state_time;
    AnimGraphSystem::evaluate(registry, true);
    const auto& pose = registry.get<AnimPoseComponent>(entity);
    if (!pose.pose_valid || pose.bone_matrices.size() != 3 ||
        !near(pose.local_pose[0].translation.x, 2.0f) ||
        !near(state.machines[0].state_time, before_evaluate))
        return fail(name, "evaluation changed graph state or did not sample the fixed-tick clock");

    AnimGraphSystem::advance(registry, -0.1f);
    AnimGraphSystem::advance(registry, std::numeric_limits<float>::infinity());
    if (!near(state.machines[0].state_time, before_evaluate))
        return fail(name, "invalid fixed deltas corrupted deterministic state");
    state.machines[0].state_time = std::numeric_limits<float>::max();
    AnimGraphSystem::advance(registry, 1.0f);
    if (!near(state.machines[0].state_time, 0.0f))
        return fail(name, "overflowing graph clock did not reset safely");
    return pass(name);
}

bool testGraphDeterministicReplayAndReinitialize()
{
    const std::string name = "AN-3 graph replay is deterministic and graph replacement resets at tick boundary";
    const auto resources = makeResources(makeRestSkeleton(), {makeGraphClip()});
    const auto graph = makeGraphFoundation(resources);
    if (!graph) return fail(name, "could not create validated graph");

    entt::registry first, second;
    const entt::entity a = first.create(), b = second.create();
    first.emplace<AnimGraphComponent>(a).graph = graph;
    first.emplace<AnimGraphStateComponent>(a);
    second.emplace<AnimGraphComponent>(b).graph = graph;
    second.emplace<AnimGraphStateComponent>(b);
    for (const float dt : {0.1f, 0.2f, 0.15f, 0.3f})
    {
        AnimGraphSystem::advance(first, dt);
        AnimGraphSystem::advance(second, dt);
        AnimGraphSystem::evaluate(first, false);
        AnimGraphSystem::evaluate(second, false);
    }
    const auto& left = first.get<AnimGraphStateComponent>(a);
    const auto& right = second.get<AnimGraphStateComponent>(b);
    if (!near(left.machines[0].state_time, right.machines[0].state_time) ||
        !nearPose(first.get<AnimPoseComponent>(a).local_pose, second.get<AnimPoseComponent>(b).local_pose))
        return fail(name, "identical fixed tick replay produced different state or pose");

    AnimGraphDefinition replacement_definition;
    replacement_definition.resource_signature = resources->sourceSignature();
    replacement_definition.states = {{"rest", 0, AnimGraphPoseSource::RestPose, "", 1.0f, true}};
    replacement_definition.machines = {{"locomotion", 0}};
    AnimGraphValidationReport report;
    const auto replacement = AnimGraphValidation::makeGraph(std::move(replacement_definition), resources, report);
    if (!replacement) return fail(name, "could not create replacement graph");
    first.get<AnimGraphComponent>(a).graph = replacement;
    const AnimGraph* previous_graph = left.initialized_graph;
    AnimGraphSystem::evaluate(first, false);
    if (first.get<AnimGraphStateComponent>(a).initialized_graph != previous_graph ||
        !near(first.get<AnimPoseComponent>(a).local_pose[0].translation.x, 3.0f))
        return fail(name, "presentation evaluation applied a replacement schema before a fixed tick");
    AnimGraphSystem::advance(first, 0.1f);
    const auto& reset = first.get<AnimGraphStateComponent>(a);
    if (reset.initialized_graph != replacement.get() || !near(reset.machines[0].state_time, 0.1f) ||
        !reset.parameters.empty())
        return fail(name, "graph replacement did not reset runtime schema at a fixed-tick boundary");
    return pass(name);
}

std::vector<AnimationNotifyEvent> g_graph_notifies;
std::vector<AnimationStateChangedEvent> g_graph_state_changes;
void onGraphNotify(const AnimationNotifyEvent& event) { g_graph_notifies.push_back(event); }
void onGraphStateChanged(const AnimationStateChangedEvent& event) { g_graph_state_changes.push_back(event); }

bool testGraphTransitionsAndEvents()
{
    const std::string name = "AN-3 graph transitions follow authored order and queue opaque events";
    const auto resources = makeResources(makeRestSkeleton(), {makeGraphClip()});
    if (!resources) return fail(name, "could not build immutable animation resources");

    AnimGraphDefinition definition;
    definition.resource_signature = resources->sourceSignature();
    definition.parameters = {
        {"go", AnimGraphParameterType::Bool, false},
        {"speed", AnimGraphParameterType::Float, 0.0f},
    };
    definition.states = {
        {"idle", 0, AnimGraphPoseSource::Clip, "graph_move", 1.0f, true},
        {"walk", 0, AnimGraphPoseSource::Clip, "graph_move", 1.0f, true},
        {"run", 0, AnimGraphPoseSource::Clip, "graph_move", 2.0f, true},
    };
    definition.machines = {{"locomotion", 0}};
    definition.transitions = {
        {0, 1, 0.5f, -1.0f, {{"go", AnimGraphConditionOperator::IsTrue, true}}},
        {0, 2, 0.5f, -1.0f, {{"go", AnimGraphConditionOperator::IsTrue, true}}},
        {1, 2, 0.75f, -1.0f, {{"speed", AnimGraphConditionOperator::Greater, 5.0f}}},
        {2, 2, 0.25f, -1.0f, {{"speed", AnimGraphConditionOperator::Greater, 100.0f}}},
    };
    definition.notifies = {{"graph_move", 0.5f, "footstep"}};
    AnimGraphValidationReport report;
    const auto graph = AnimGraphValidation::makeGraph(std::move(definition), resources, report);
    if (!graph || !report.valid()) return fail(name, "could not publish a valid transition graph");

    EventBus::get().clear();
    EventBus::get().sink<AnimationNotifyEvent>().connect<&onGraphNotify>();
    EventBus::get().sink<AnimationStateChangedEvent>().connect<&onGraphStateChanged>();
    g_graph_notifies.clear();
    g_graph_state_changes.clear();
    entt::registry registry;
    const entt::entity entity = registry.create();
    registry.emplace<AnimGraphComponent>(entity).graph = graph;
    registry.emplace<AnimGraphStateComponent>(entity);
    AnimGraphSystem::advance(registry, 0.3f);
    if (!AnimGraphSystem::setBool(registry, entity, "go", true))
        return fail(name, "could not set valid Bool transition parameter");
    AnimGraphSystem::advance(registry, 0.3f);
    auto& runtime = registry.get<AnimGraphStateComponent>(entity);
    if (runtime.machines[0].active_state != 1 || runtime.machines[0].fading_from_state != 0 ||
        !near(runtime.machines[0].fading_from_time, 0.6f) || !near(runtime.machines[0].state_time, 0.0f))
        return fail(name, "first authored transition did not win or did not start a deterministic fade");

    if (!AnimGraphSystem::setFloat(registry, entity, "speed", 6.0f))
        return fail(name, "could not set valid Float transition parameter");
    AnimGraphSystem::advance(registry, 0.2f);
    if (runtime.machines[0].active_state != 2 || runtime.machines[0].fading_from_state != 1 ||
        !near(runtime.machines[0].fading_from_time, 0.2f) || !near(runtime.machines[0].fade_duration, 0.75f))
        return fail(name, "interrupted fade did not use the previous destination as its source");
    AnimGraphSystem::evaluate(registry, false);
    if (runtime.machines[0].active_state != 2 || !near(runtime.machines[0].state_time, 0.0f))
        return fail(name, "presentation evaluation mutated transition state");
    if (!AnimGraphSystem::setFloat(registry, entity, "speed", 101.0f))
        return fail(name, "could not set self-transition parameter");
    AnimGraphSystem::advance(registry, 0.1f);
    if (runtime.machines[0].active_state != 2 || runtime.machines[0].fading_from_state != 2 ||
        !near(runtime.machines[0].state_time, 0.0f) || !near(runtime.machines[0].fading_from_time, 0.1f))
        return fail(name, "self transition did not restart the entered state deterministically");

    EventBus::get().flush();
    EventBus::get().sink<AnimationNotifyEvent>().disconnect<&onGraphNotify>();
    EventBus::get().sink<AnimationStateChangedEvent>().disconnect<&onGraphStateChanged>();
    EventBus::get().clear();
    if (g_graph_state_changes.size() != 3 || g_graph_state_changes[0].machine != "locomotion" ||
        g_graph_state_changes[0].from_state != "idle" || g_graph_state_changes[0].to_state != "walk" ||
        g_graph_state_changes[1].from_state != "walk" || g_graph_state_changes[1].to_state != "run" ||
        g_graph_state_changes[2].from_state != "run" || g_graph_state_changes[2].to_state != "run" ||
        g_graph_notifies.size() != 1 || g_graph_notifies[0].name != "footstep" ||
        g_graph_notifies[0].state != 0 || !near(g_graph_notifies[0].clip_time, 0.5f))
        return fail(name, "graph did not queue the expected authored state and opaque notify events");
    return pass(name);
}

bool testGraphTransitionValidationAndExitGate()
{
    const std::string name = "AN-3 graph rejects invalid conditions and gates normalized exits";
    const auto resources = makeResources(makeRestSkeleton(), {makeGraphClip()});
    if (!resources) return fail(name, "could not build immutable animation resources");
    AnimGraphDefinition invalid;
    invalid.resource_signature = resources->sourceSignature();
    invalid.parameters = {{"go", AnimGraphParameterType::Bool, false}};
    invalid.states = {{"idle", 0, AnimGraphPoseSource::Clip, "graph_move", 1.0f, true},
                      {"rest", 0, AnimGraphPoseSource::RestPose, "", 1.0f, true}};
    invalid.machines = {{"base", 0}};
    invalid.transitions = {{0, 1, -1.0f, 2.0f, {{"go", AnimGraphConditionOperator::Less, false}}}};
    invalid.notifies = {{"missing", 1.1f, ""}};
    AnimGraphValidationReport report;
    if (AnimGraphValidation::makeGraph(invalid, resources, report) || report.errors.size() < 5)
        return fail(name, "invalid current graph contract was published or under-reported");

    AnimGraphDefinition gated;
    gated.resource_signature = resources->sourceSignature();
    gated.parameters = {{"go", AnimGraphParameterType::Bool, true}};
    gated.states = invalid.states;
    gated.machines = {{"base", 0}};
    gated.transitions = {{0, 1, 0.0f, 0.5f, {{"go", AnimGraphConditionOperator::IsTrue, true}}}};
    const auto graph = AnimGraphValidation::makeGraph(std::move(gated), resources, report);
    if (!graph) return fail(name, "valid exit-gated graph was rejected");
    entt::registry registry;
    const entt::entity entity = registry.create();
    registry.emplace<AnimGraphComponent>(entity).graph = graph;
    registry.emplace<AnimGraphStateComponent>(entity);
    AnimGraphSystem::advance(registry, 0.4f);
    if (AnimGraphSystem::activeState(registry, entity, "base") != 0)
        return fail(name, "transition fired before normalized exit time");
    AnimGraphSystem::advance(registry, 0.1f);
    if (AnimGraphSystem::activeState(registry, entity, "base") != 1)
        return fail(name, "transition did not fire at normalized exit time");
    return pass(name);
}
} // namespace

int main()
{
    bool ok = true;
    ok = testCanmSerialization() && ok;
    ok = testResourceServiceAndBinding() && ok;
    ok = testImmutableResourceValidation() && ok;
    ok = testCheckedGltfAnimationSource() && ok;
    ok = testRestPoseAndOverlay() && ok;
    ok = testSystemRestAndInvalidPlayback() && ok;
    ok = testIkOrder() && ok;
    ok = testCompletionEvent() && ok;
    ok = testReflectionHardCutover() && ok;
    ok = testGraphFoundationValidation() && ok;
    ok = testGraphFixedTickAndEvaluation() && ok;
    ok = testGraphDeterministicReplayAndReinitialize() && ok;
    ok = testGraphTransitionsAndEvents() && ok;
    ok = testGraphTransitionValidationAndExitGate() && ok;
    return ok ? 0 : 1;
}
