#include "GltfAnimationLoader.hpp"
#include "AnimationResourceValidation.hpp"
#include "Utils/Log.hpp"
#include "Utils/TangentGenerator.hpp"

// Must include the full tinygltf header in the .cpp
#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace GltfAnimationLoader
{

namespace
{
void fail(AnimationResourceError& error, const char* code, std::string message)
{
    error.code = code;
    error.message = std::move(message);
}

bool checkedAdd(size_t left, size_t right, size_t& result)
{
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool checkedMultiply(size_t left, size_t right, size_t& result)
{
    if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
    result = left * right;
    return true;
}

struct AnimationAccessorView
{
    const unsigned char* bytes = nullptr;
    size_t count = 0;
    size_t elements = 0;
};

bool readAnimationAccessor(const tinygltf::Model& model, int accessor_index,
                           int expected_type, AnimationAccessorView& out,
                           AnimationResourceError& error)
{
    out = {};
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size()))
    {
        fail(error, "ANIM_ACCESSOR_INDEX", "animation accessor index is out of range");
        return false;
    }
    const auto& accessor = model.accessors[accessor_index];
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != expected_type)
    {
        fail(error, "ANIM_ACCESSOR_TYPE", "animation accessor must have the required FLOAT element type");
        return false;
    }
    if (accessor.sparse.isSparse)
    {
        fail(error, "ANIM_ACCESSOR_SPARSE", "sparse animation accessors are unsupported");
        return false;
    }
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        fail(error, "ANIM_ACCESSOR_VIEW", "animation accessor buffer view is out of range");
        return false;
    }
    const auto& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
    {
        fail(error, "ANIM_ACCESSOR_BUFFER", "animation accessor buffer is out of range");
        return false;
    }
    const int component_count = tinygltf::GetNumComponentsInType(accessor.type);
    const size_t elements = component_count > 0 ? static_cast<size_t>(component_count) : 0;
    size_t element_bytes = 0;
    if (elements == 0 || !checkedMultiply(elements, sizeof(float), element_bytes))
    {
        fail(error, "ANIM_ACCESSOR_LAYOUT", "animation accessor element shape is invalid");
        return false;
    }
    if (view.byteStride != 0 && view.byteStride != element_bytes)
    {
        fail(error, "ANIM_ACCESSOR_LAYOUT", "interleaved animation accessors are unsupported");
        return false;
    }
    size_t data_offset = 0;
    size_t byte_count = 0;
    size_t end_offset = 0;
    size_t view_end = 0;
    if (!checkedAdd(view.byteOffset, accessor.byteOffset, data_offset) ||
        !checkedMultiply(accessor.count, element_bytes, byte_count) ||
        !checkedAdd(data_offset, byte_count, end_offset) ||
        !checkedAdd(view.byteOffset, view.byteLength, view_end) ||
        end_offset > view_end || end_offset > model.buffers[view.buffer].data.size())
    {
        fail(error, "ANIM_ACCESSOR_RANGE", "animation accessor byte range is outside its buffer view");
        return false;
    }
    out.bytes = model.buffers[view.buffer].data.data() + data_offset;
    out.count = accessor.count;
    out.elements = elements;
    return true;
}

float accessorFloat(const AnimationAccessorView& view, size_t index, size_t element = 0)
{
    float value = 0.0f;
    std::memcpy(&value, view.bytes + (index * view.elements + element) * sizeof(float), sizeof(value));
    return value;
}

bool finite(double value) { return std::isfinite(value); }
bool finite(float value) { return std::isfinite(value); }

bool makeNodeLocalTransform(const tinygltf::Node& node, glm::mat4& out, AnimationResourceError& error)
{
    if (!node.matrix.empty())
    {
        if (node.matrix.size() != 16 || !node.translation.empty() || !node.rotation.empty() || !node.scale.empty())
        {
            fail(error, "ANIM_BONE_INVALID_REST", "joint node matrix/TRS representation is invalid");
            return false;
        }
        float values[16];
        for (size_t index = 0; index < 16; ++index)
        {
            if (!finite(node.matrix[index]))
            {
                fail(error, "ANIM_BONE_NONFINITE_MATRIX", "joint node matrix contains non-finite values");
                return false;
            }
            values[index] = static_cast<float>(node.matrix[index]);
            if (!finite(values[index]))
            {
                fail(error, "ANIM_BONE_NONFINITE_MATRIX", "joint node matrix exceeds finite float range");
                return false;
            }
        }
        out = glm::make_mat4(values);
        return true;
    }
    if ((!node.translation.empty() && node.translation.size() != 3) ||
        (!node.rotation.empty() && node.rotation.size() != 4) ||
        (!node.scale.empty() && node.scale.size() != 3))
    {
        fail(error, "ANIM_BONE_INVALID_REST", "joint node TRS element counts are invalid");
        return false;
    }
    glm::vec3 translation(0.0f), scale(1.0f);
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
    for (size_t index = 0; index < node.translation.size(); ++index)
    {
        if (!finite(node.translation[index])) { fail(error, "ANIM_BONE_INVALID_REST", "joint translation is non-finite"); return false; }
        translation[static_cast<int>(index)] = static_cast<float>(node.translation[index]);
    }
    for (size_t index = 0; index < node.scale.size(); ++index)
    {
        if (!finite(node.scale[index])) { fail(error, "ANIM_BONE_INVALID_REST", "joint scale is non-finite"); return false; }
        scale[static_cast<int>(index)] = static_cast<float>(node.scale[index]);
    }
    if (!node.rotation.empty())
    {
        for (double value : node.rotation)
            if (!finite(value)) { fail(error, "ANIM_BONE_INVALID_REST", "joint rotation is non-finite"); return false; }
        rotation = glm::quat(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                             static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]));
        const float length = glm::length(rotation);
        if (!finite(length) || length <= 0.0f)
        {
            fail(error, "ANIM_BONE_INVALID_REST", "joint rotation has zero length");
            return false;
        }
        rotation = glm::normalize(rotation);
    }
    out = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
    return true;
}

bool readAnimationTimes(const AnimationAccessorView& input, std::vector<float>& times,
                        AnimationResourceError& error)
{
    if (input.count == 0)
    {
        fail(error, "ANIM_SAMPLER_COUNT", "animation sampler has no keys");
        return false;
    }
    times.resize(input.count);
    float previous = -1.0f;
    for (size_t index = 0; index < input.count; ++index)
    {
        const float value = accessorFloat(input, index);
        if (!finite(value) || value < 0.0f || (index > 0 && value <= previous))
        {
            fail(error, "ANIM_INVALID_KEY", "animation times must be finite, non-negative, and strictly increasing");
            return false;
        }
        times[index] = value;
        previous = value;
    }
    return true;
}

template<typename Key, typename Value>
void appendKeys(const std::vector<float>& times, const AnimationAccessorView& values,
                std::vector<Key>& keys, Value make_value)
{
    keys.reserve(keys.size() + times.size());
    for (size_t index = 0; index < times.size(); ++index)
        keys.push_back({times[index], make_value(values, index)});
}

// Legacy mesh extraction deliberately remains separate from AN-2's checked animation path.
template<typename T>
const T* getLegacyAccessorData(const tinygltf::Model& model, int accessor_index, size_t& count)
{
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size())) { count = 0; return nullptr; }
    const auto& accessor = model.accessors[accessor_index];
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) { count = 0; return nullptr; }
    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    if (buffer_view.buffer < 0 || buffer_view.buffer >= static_cast<int>(model.buffers.size())) { count = 0; return nullptr; }
    const auto& buffer = model.buffers[buffer_view.buffer];
    count = accessor.count;
    return reinterpret_cast<const T*>(buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset);
}
} // namespace

std::shared_ptr<const Skeleton> loadSkeleton(const tinygltf::Model& model, int skin_index,
                                             AnimationResourceError* out_error)
{
    AnimationResourceError error;
    auto reject = [&](const char* code, std::string message) {
        fail(error, code, std::move(message));
        if (out_error) *out_error = error;
        LOG_ENGINE_WARN("Rejected glTF skeleton: {}", error.code);
        return std::shared_ptr<const Skeleton>{};
    };
    if (skin_index < 0 || skin_index >= static_cast<int>(model.skins.size()))
        return reject("ANIM_SKIN_INDEX", "selected skin index is out of range");
    const auto& skin = model.skins[skin_index];
    if (skin.joints.empty() || skin.joints.size() > static_cast<size_t>(MAX_BONES))
        return reject("ANIM_BONE_COUNT", "skin joint count must be in 1..MAX_BONES");

    std::vector<int> node_parents(model.nodes.size(), -1);
    for (size_t parent = 0; parent < model.nodes.size(); ++parent)
    {
        for (int child : model.nodes[parent].children)
        {
            if (child < 0 || child >= static_cast<int>(model.nodes.size()))
                return reject("ANIM_BONE_NODE", "node hierarchy contains an invalid child index");
            if (node_parents[child] != -1)
                return reject("ANIM_BONE_HIERARCHY", "joint hierarchy contains a node with multiple parents");
            node_parents[child] = static_cast<int>(parent);
        }
    }
    std::unordered_map<int, int> node_to_joint;
    for (size_t index = 0; index < skin.joints.size(); ++index)
    {
        const int node_index = skin.joints[index];
        if (node_index < 0 || node_index >= static_cast<int>(model.nodes.size()))
            return reject("ANIM_BONE_NODE", "skin joint node index is out of range");
        if (!node_to_joint.emplace(node_index, static_cast<int>(index)).second)
            return reject("ANIM_BONE_DUPLICATE", "skin lists one joint node more than once");
    }

    std::vector<glm::mat4> inverse_bind_matrices(skin.joints.size(), glm::mat4(1.0f));
    if (skin.inverseBindMatrices >= 0)
    {
        AnimationAccessorView bind_data;
        if (!readAnimationAccessor(model, skin.inverseBindMatrices, TINYGLTF_TYPE_MAT4, bind_data, error))
        {
            if (out_error) *out_error = error;
            return nullptr;
        }
        if (bind_data.count != skin.joints.size())
            return reject("ANIM_BIND_COUNT", "inverse bind matrix count must exactly match skin joints");
        for (size_t index = 0; index < bind_data.count; ++index)
        {
            float values[16];
            for (size_t element = 0; element < 16; ++element)
            {
                values[element] = accessorFloat(bind_data, index, element);
                if (!finite(values[element])) return reject("ANIM_BONE_NONFINITE_MATRIX", "inverse bind matrix contains non-finite values");
            }
            inverse_bind_matrices[index] = glm::make_mat4(values);
        }
    }

    std::vector<Bone> bones;
    bones.reserve(skin.joints.size());
    for (size_t index = 0; index < skin.joints.size(); ++index)
    {
        const int node_index = skin.joints[index];
        Bone bone;
        bone.id = static_cast<int>(index);
        bone.name = model.nodes[node_index].name;
        bone.inverse_bind_matrix = inverse_bind_matrices[index];
        if (!makeNodeLocalTransform(model.nodes[node_index], bone.local_transform, error))
        {
            if (out_error) *out_error = error;
            return nullptr;
        }
        const int parent_node = node_parents[node_index];
        if (parent_node >= 0)
        {
            const auto parent = node_to_joint.find(parent_node);
            if (parent == node_to_joint.end())
                return reject("ANIM_BONE_HIERARCHY", "joint has a non-joint immediate parent");
            if (parent->second >= static_cast<int>(index))
                return reject("ANIM_BONE_PARENT_ORDER", "joint parent must precede child in skin joint order");
            bone.parent_id = parent->second;
        }
        bones.push_back(std::move(bone));
    }
    auto skeleton = AnimationResourceValidation::makeSkeleton(std::move(bones), error);
    if (!skeleton)
    {
        if (out_error) *out_error = error;
        LOG_ENGINE_WARN("Rejected glTF skeleton: {}", error.code);
        return nullptr;
    }
    if (out_error) *out_error = {};
    LOG_ENGINE_INFO("Loaded skeleton with {} bones from skin '{}'", skeleton->getBoneCount(), skin.name);
    return skeleton;
}

std::vector<std::shared_ptr<const AnimationClip>> loadAnimations(
    const tinygltf::Model& model, const Skeleton& skeleton, int skin_index,
    AnimationResourceError* out_error)
{
    AnimationResourceError error;
    auto reject = [&](const char* code, std::string message) {
        fail(error, code, std::move(message));
        if (out_error) *out_error = error;
        LOG_ENGINE_WARN("Rejected glTF animation: {}", error.code);
        return std::vector<std::shared_ptr<const AnimationClip>>{};
    };
    if (skin_index < 0 || skin_index >= static_cast<int>(model.skins.size()))
        return reject("ANIM_SKIN_INDEX", "selected skin index is out of range");
    const auto& skin = model.skins[skin_index];
    if (skeleton.getBoneCount() != static_cast<int>(skin.joints.size()))
        return reject("ANIM_RESOURCE_SKELETON", "skeleton does not match the selected skin");
    std::unordered_map<int, int> node_to_bone;
    for (size_t index = 0; index < skin.joints.size(); ++index)
        node_to_bone.emplace(skin.joints[index], static_cast<int>(index));

    std::unordered_set<std::string> clip_names;
    std::vector<std::shared_ptr<const AnimationClip>> clips;
    for (const auto& anim : model.animations)
    {
        if (anim.name.empty() || !clip_names.emplace(anim.name).second)
            return reject("ANIM_CLIP_NAME", "animation names must be non-empty and unique");
        AnimationClipDefinition clip;
        clip.name = anim.name;
        clip.duration = 0.0f;
        std::unordered_map<int, size_t> channel_by_bone;
        std::unordered_set<std::string> keyed_properties;
        for (const auto& channel : anim.channels)
        {
            if (channel.target_node < 0 || channel.target_node >= static_cast<int>(model.nodes.size()))
                return reject("ANIM_CHANNEL_TARGET", "animation channel targets an invalid node");
            if (channel.sampler < 0 || channel.sampler >= static_cast<int>(anim.samplers.size()))
                return reject("ANIM_SAMPLER_INDEX", "animation channel sampler index is out of range");
            if (channel.target_path != "translation" && channel.target_path != "rotation" && channel.target_path != "scale")
                return reject("ANIM_CHANNEL_TARGET", "animation channel targets an unsupported property");
            const auto bone = node_to_bone.find(channel.target_node);
            if (bone == node_to_bone.end())
            {
                LOG_ENGINE_WARN("ANIM_UNKNOWN_BONE_CHANNEL: animation '{}' targets node {} outside selected skin", anim.name, channel.target_node);
                continue;
            }
            const std::string property_key = std::to_string(bone->second) + ":" + channel.target_path;
            if (!keyed_properties.emplace(property_key).second)
                return reject("ANIM_DUPLICATE_CHANNEL", "animation repeats a property for one bone");
            const auto& sampler = anim.samplers[channel.sampler];
            if (sampler.interpolation != "LINEAR")
                return reject("ANIM_UNSUPPORTED_INTERPOLATION", "animation sampler interpolation must be LINEAR");
            AnimationAccessorView times_data, values_data;
            const int expected_output = channel.target_path == "rotation" ? TINYGLTF_TYPE_VEC4 : TINYGLTF_TYPE_VEC3;
            if (!readAnimationAccessor(model, sampler.input, TINYGLTF_TYPE_SCALAR, times_data, error) ||
                !readAnimationAccessor(model, sampler.output, expected_output, values_data, error))
            {
                if (out_error) *out_error = error;
                return {};
            }
            if (times_data.count != values_data.count)
                return reject("ANIM_SAMPLER_COUNT", "animation sampler input and output key counts differ");
            std::vector<float> times;
            if (!readAnimationTimes(times_data, times, error))
            {
                if (out_error) *out_error = error;
                return {};
            }
            auto [channel_it, inserted] = channel_by_bone.emplace(bone->second, clip.channels.size());
            if (inserted)
            {
                BoneAnimation animation_channel;
                animation_channel.bone_index = bone->second;
                animation_channel.bone_name = skeleton.getBone(bone->second)->name;
                clip.channels.push_back(std::move(animation_channel));
            }
            BoneAnimation& output = clip.channels[channel_it->second];
            if (channel.target_path == "translation")
            {
                for (size_t index = 0; index < values_data.count; ++index)
                    for (size_t element = 0; element < 3; ++element)
                        if (!finite(accessorFloat(values_data, index, element))) return reject("ANIM_INVALID_KEY", "translation key contains non-finite data");
                appendKeys<PositionKey>(times, values_data, output.position_keys, [](const auto& data, size_t index) {
                    return glm::vec3(accessorFloat(data, index, 0), accessorFloat(data, index, 1), accessorFloat(data, index, 2)); });
            }
            else if (channel.target_path == "scale")
            {
                for (size_t index = 0; index < values_data.count; ++index)
                    for (size_t element = 0; element < 3; ++element)
                        if (!finite(accessorFloat(values_data, index, element))) return reject("ANIM_INVALID_KEY", "scale key contains non-finite data");
                appendKeys<ScaleKey>(times, values_data, output.scale_keys, [](const auto& data, size_t index) {
                    return glm::vec3(accessorFloat(data, index, 0), accessorFloat(data, index, 1), accessorFloat(data, index, 2)); });
            }
            else
            {
                for (size_t index = 0; index < values_data.count; ++index)
                {
                    glm::quat value(accessorFloat(values_data, index, 3), accessorFloat(values_data, index, 0),
                                    accessorFloat(values_data, index, 1), accessorFloat(values_data, index, 2));
                    const float length = glm::length(value);
                    if (!finite(value.w) || !finite(value.x) || !finite(value.y) || !finite(value.z) || !finite(length) || length <= 0.0f)
                        return reject("ANIM_INVALID_KEY", "rotation key must be finite and non-zero");
                    value = glm::normalize(value);
                    output.rotation_keys.push_back({times[index], value});
                }
            }
            clip.duration = std::max(clip.duration, times.back());
        }
        AnimationResourceError validation_error;
        auto immutable_clip = AnimationResourceValidation::makeClip(std::move(clip), skeleton.getBoneCount(), validation_error);
        if (!immutable_clip)
        {
            if (out_error) *out_error = validation_error;
            LOG_ENGINE_WARN("Rejected glTF animation: {}", validation_error.code);
            return {};
        }
        LOG_ENGINE_INFO("Loaded animation '{}' ({:.2f}s, {} channels)", immutable_clip->name(), immutable_clip->duration(), immutable_clip->channels().size());
        clips.push_back(std::move(immutable_clip));
    }
    if (out_error) *out_error = {};
    return clips;
}

bool loadSkinnedVertices(const tinygltf::Model& model, int mesh_index,
                          std::vector<skinned_vertex>& out_vertices)
{
    if (mesh_index < 0 || mesh_index >= static_cast<int>(model.meshes.size()))
        return false;

    const auto& gltf_mesh = model.meshes[mesh_index];
    out_vertices.clear();

    for (const auto& primitive : gltf_mesh.primitives)
    {
        // Find required attributes
        auto pos_it = primitive.attributes.find("POSITION");
        if (pos_it == primitive.attributes.end()) continue;

        size_t vertex_count;
        const float* positions = getLegacyAccessorData<float>(model, pos_it->second, vertex_count);
        if (!positions) continue;

        // Optional attributes
        size_t normal_count = 0, texcoord_count = 0, tangent_count = 0, joints_count = 0, weights_count = 0;
        const float* normals = nullptr;
        const float* texcoords = nullptr;
        const float* tangent_data = nullptr;
        const unsigned char* joints_u8 = nullptr;
        const unsigned short* joints_u16 = nullptr;
        const float* weights = nullptr;
        bool joints_are_u16 = false;
        bool has_tangents = false;

        auto norm_it = primitive.attributes.find("NORMAL");
        if (norm_it != primitive.attributes.end())
            normals = getLegacyAccessorData<float>(model, norm_it->second, normal_count);

        auto tex_it = primitive.attributes.find("TEXCOORD_0");
        if (tex_it != primitive.attributes.end())
            texcoords = getLegacyAccessorData<float>(model, tex_it->second, texcoord_count);

        auto tan_it = primitive.attributes.find("TANGENT");
        if (tan_it != primitive.attributes.end())
        {
            tangent_data = getLegacyAccessorData<float>(model, tan_it->second, tangent_count);
            has_tangents = (tangent_data != nullptr && tangent_count == vertex_count);
        }

        auto joints_it = primitive.attributes.find("JOINTS_0");
        if (joints_it != primitive.attributes.end())
        {
            const auto& accessor = model.accessors[joints_it->second];
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                joints_u16 = getLegacyAccessorData<unsigned short>(model, joints_it->second, joints_count);
                joints_are_u16 = true;
            }
            else
            {
                joints_u8 = getLegacyAccessorData<unsigned char>(model, joints_it->second, joints_count);
            }
        }

        auto weights_it = primitive.attributes.find("WEIGHTS_0");
        if (weights_it != primitive.attributes.end())
            weights = getLegacyAccessorData<float>(model, weights_it->second, weights_count);

        // Build skinned vertices
        size_t base = out_vertices.size();
        out_vertices.resize(base + vertex_count);

        for (size_t i = 0; i < vertex_count; i++)
        {
            skinned_vertex& sv = out_vertices[base + i];

            sv.vx = positions[i * 3 + 0];
            sv.vy = positions[i * 3 + 1];
            sv.vz = positions[i * 3 + 2];

            if (normals && i < normal_count)
            {
                sv.nx = normals[i * 3 + 0];
                sv.ny = normals[i * 3 + 1];
                sv.nz = normals[i * 3 + 2];
            }
            else
            {
                sv.nx = sv.ny = sv.nz = 0.0f;
            }

            if (texcoords && i < texcoord_count)
            {
                sv.u = texcoords[i * 2 + 0];
                sv.v = texcoords[i * 2 + 1];
            }
            else
            {
                sv.u = sv.v = 0.0f;
            }

            if (has_tangents)
            {
                sv.tx = tangent_data[i * 4 + 0];
                sv.ty = tangent_data[i * 4 + 1];
                sv.tz = tangent_data[i * 4 + 2];
                sv.tw = tangent_data[i * 4 + 3];
            }
            else
            {
                sv.tx = 1.0f; sv.ty = 0.0f; sv.tz = 0.0f; sv.tw = 1.0f;
            }

            // Bone influences
            if (joints_count > 0 && i < joints_count)
            {
                if (joints_are_u16 && joints_u16)
                {
                    sv.bone_ids[0] = static_cast<int>(joints_u16[i * 4 + 0]);
                    sv.bone_ids[1] = static_cast<int>(joints_u16[i * 4 + 1]);
                    sv.bone_ids[2] = static_cast<int>(joints_u16[i * 4 + 2]);
                    sv.bone_ids[3] = static_cast<int>(joints_u16[i * 4 + 3]);
                }
                else if (joints_u8)
                {
                    sv.bone_ids[0] = static_cast<int>(joints_u8[i * 4 + 0]);
                    sv.bone_ids[1] = static_cast<int>(joints_u8[i * 4 + 1]);
                    sv.bone_ids[2] = static_cast<int>(joints_u8[i * 4 + 2]);
                    sv.bone_ids[3] = static_cast<int>(joints_u8[i * 4 + 3]);
                }
            }
            else
            {
                sv.bone_ids[0] = sv.bone_ids[1] = sv.bone_ids[2] = sv.bone_ids[3] = 0;
            }

            if (weights && i < weights_count)
            {
                sv.bone_weights[0] = weights[i * 4 + 0];
                sv.bone_weights[1] = weights[i * 4 + 1];
                sv.bone_weights[2] = weights[i * 4 + 2];
                sv.bone_weights[3] = weights[i * 4 + 3];
            }
            else
            {
                sv.bone_weights[0] = 1.0f;
                sv.bone_weights[1] = sv.bone_weights[2] = sv.bone_weights[3] = 0.0f;
            }
        }

        // Generate tangents from geometry if not present in glTF data
        if (!has_tangents && vertex_count >= 3)
        {
            TangentGenerator::generateSkinned(&out_vertices[base], vertex_count);
        }
    }

    LOG_ENGINE_INFO("Loaded {} skinned vertices from mesh '{}'",
        out_vertices.size(), gltf_mesh.name);

    return !out_vertices.empty();
}

bool load(const tinygltf::Model& model, GltfAnimationData& out_data,
          AnimationResourceError* out_error)
{
    out_data = {};
    if (out_error) *out_error = {};
    if (model.skins.empty())
    {
        out_data.has_animation = false;
        return true;
    }

    AnimationResourceError error;
    out_data.skeleton = loadSkeleton(model, 0, &error);
    if (!out_data.skeleton)
    {
        if (out_error) *out_error = error;
        return false;
    }
    out_data.clips = loadAnimations(model, *out_data.skeleton, 0, &error);
    if (!error.code.empty())
    {
        out_data = {};
        if (out_error) *out_error = error;
        return false;
    }
    for (const auto& node : model.nodes)
    {
        if (node.skin == 0 && node.mesh >= 0)
        {
            loadSkinnedVertices(model, node.mesh, out_data.skinned_vertices);
            break;
        }
    }
    out_data.has_animation = !out_data.clips.empty();
    return true;
}

} // namespace GltfAnimationLoader
