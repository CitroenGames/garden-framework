#include "GltfAnimationLoader.hpp"
#include "Utils/Log.hpp"

// Must include the full tinygltf header in the .cpp
#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GltfAnimationLoader
{

// Helper: get accessor data as a typed pointer
template<typename T>
static const T* getAccessorData(const tinygltf::Model& model, int accessor_index, size_t& count)
{
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size()))
    {
        count = 0;
        return nullptr;
    }

    const auto& accessor = model.accessors[accessor_index];
    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[buffer_view.buffer];

    count = accessor.count;
    return reinterpret_cast<const T*>(
        buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset);
}

// Helper: compute local transform matrix from a glTF node
static glm::mat4 getNodeLocalTransform(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        return glm::make_mat4(node.matrix.data());
    }

    glm::mat4 t(1.0f), r(1.0f), s(1.0f);

    if (node.translation.size() == 3)
    {
        t = glm::translate(glm::mat4(1.0f), glm::vec3(
            static_cast<float>(node.translation[0]),
            static_cast<float>(node.translation[1]),
            static_cast<float>(node.translation[2])));
    }

    if (node.rotation.size() == 4)
    {
        // glTF stores quaternion as (x, y, z, w)
        glm::quat q(
            static_cast<float>(node.rotation[3]),  // w
            static_cast<float>(node.rotation[0]),  // x
            static_cast<float>(node.rotation[1]),  // y
            static_cast<float>(node.rotation[2])); // z
        r = glm::mat4_cast(q);
    }

    if (node.scale.size() == 3)
    {
        s = glm::scale(glm::mat4(1.0f), glm::vec3(
            static_cast<float>(node.scale[0]),
            static_cast<float>(node.scale[1]),
            static_cast<float>(node.scale[2])));
    }

    return t * r * s;
}

std::shared_ptr<Skeleton> loadSkeleton(const tinygltf::Model& model, int skin_index)
{
    if (skin_index < 0 || skin_index >= static_cast<int>(model.skins.size()))
        return nullptr;

    const auto& skin = model.skins[skin_index];
    auto skeleton = std::make_shared<Skeleton>();

    // Load inverse bind matrices
    std::vector<glm::mat4> inverse_bind_matrices;
    if (skin.inverseBindMatrices >= 0)
    {
        size_t count;
        const float* data = getAccessorData<float>(model, skin.inverseBindMatrices, count);
        if (data)
        {
            inverse_bind_matrices.resize(count);
            for (size_t i = 0; i < count; i++)
            {
                inverse_bind_matrices[i] = glm::make_mat4(data + i * 16);
            }
        }
    }

    // Build a map from node index to joint index
    std::unordered_map<int, int> node_to_joint;
    for (int i = 0; i < static_cast<int>(skin.joints.size()); i++)
    {
        node_to_joint[skin.joints[i]] = i;
    }

    // Create bones from joints
    for (int i = 0; i < static_cast<int>(skin.joints.size()); i++)
    {
        int node_index = skin.joints[i];
        const auto& node = model.nodes[node_index];

        Bone bone;
        bone.id = i;
        bone.name = node.name;
        bone.local_transform = getNodeLocalTransform(node);

        if (i < static_cast<int>(inverse_bind_matrices.size()))
        {
            bone.inverse_bind_matrix = inverse_bind_matrices[i];
        }

        // Find parent: walk node tree to find which joint is the parent
        bone.parent_id = -1;
        for (int j = 0; j < static_cast<int>(skin.joints.size()); j++)
        {
            if (j == i) continue;
            int parent_node = skin.joints[j];
            const auto& parent = model.nodes[parent_node];
            for (int child : parent.children)
            {
                if (child == node_index)
                {
                    bone.parent_id = j;
                    break;
                }
            }
            if (bone.parent_id >= 0) break;
        }

        skeleton->addBone(bone);
    }

    LOG_ENGINE_INFO("Loaded skeleton with {} bones from skin '{}'",
        skeleton->getBoneCount(), skin.name);

    return skeleton;
}

std::vector<std::shared_ptr<AnimationClip>> loadAnimations(
    const tinygltf::Model& model,
    const Skeleton& skeleton)
{
    std::vector<std::shared_ptr<AnimationClip>> clips;

    // Build node-to-bone map
    // We need the skin's joints to map node indices to bone indices
    std::unordered_map<int, int> node_to_bone;
    if (!model.skins.empty())
    {
        const auto& skin = model.skins[0];
        for (int i = 0; i < static_cast<int>(skin.joints.size()); i++)
        {
            node_to_bone[skin.joints[i]] = i;
        }
    }

    for (const auto& anim : model.animations)
    {
        auto clip = std::make_shared<AnimationClip>();
        clip->name = anim.name.empty() ? "unnamed" : anim.name;
        clip->duration = 0.0f;

        for (const auto& channel : anim.channels)
        {
            if (channel.target_node < 0) continue;

            // Find which bone this channel targets
            auto bone_it = node_to_bone.find(channel.target_node);
            if (bone_it == node_to_bone.end()) continue;

            int bone_index = bone_it->second;

            // Find or create BoneAnimation for this bone
            BoneAnimation* bone_anim = nullptr;
            for (auto& ba : clip->channels)
            {
                if (ba.bone_index == bone_index)
                {
                    bone_anim = &ba;
                    break;
                }
            }
            if (!bone_anim)
            {
                clip->channels.push_back({});
                bone_anim = &clip->channels.back();
                bone_anim->bone_index = bone_index;
                const auto* bone = skeleton.getBone(bone_index);
                if (bone) bone_anim->bone_name = bone->name;
            }

            // Get sampler
            const auto& sampler = anim.samplers[channel.sampler];

            // Read input (timestamps)
            size_t time_count;
            const float* times = getAccessorData<float>(model, sampler.input, time_count);
            if (!times) continue;

            // Track max duration
            for (size_t i = 0; i < time_count; i++)
            {
                if (times[i] > clip->duration)
                    clip->duration = times[i];
            }

            // Read output (values)
            size_t value_count;
            const float* values = getAccessorData<float>(model, sampler.output, value_count);
            if (!values) continue;

            if (channel.target_path == "translation")
            {
                bone_anim->position_keys.reserve(time_count);
                for (size_t i = 0; i < time_count; i++)
                {
                    PositionKey key;
                    key.time = times[i];
                    key.value = glm::vec3(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
                    bone_anim->position_keys.push_back(key);
                }
            }
            else if (channel.target_path == "rotation")
            {
                bone_anim->rotation_keys.reserve(time_count);
                for (size_t i = 0; i < time_count; i++)
                {
                    RotationKey key;
                    key.time = times[i];
                    // glTF: (x, y, z, w)
                    key.value = glm::quat(
                        values[i * 4 + 3],  // w
                        values[i * 4 + 0],  // x
                        values[i * 4 + 1],  // y
                        values[i * 4 + 2]); // z
                    bone_anim->rotation_keys.push_back(key);
                }
            }
            else if (channel.target_path == "scale")
            {
                bone_anim->scale_keys.reserve(time_count);
                for (size_t i = 0; i < time_count; i++)
                {
                    ScaleKey key;
                    key.time = times[i];
                    key.value = glm::vec3(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
                    bone_anim->scale_keys.push_back(key);
                }
            }
        }

        if (!clip->channels.empty())
        {
            LOG_ENGINE_INFO("Loaded animation '{}' ({:.2f}s, {} channels)",
                clip->name, clip->duration, clip->channels.size());
            clips.push_back(std::move(clip));
        }
    }

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
        const float* positions = getAccessorData<float>(model, pos_it->second, vertex_count);
        if (!positions) continue;

        // Optional attributes
        size_t normal_count = 0, texcoord_count = 0, joints_count = 0, weights_count = 0;
        const float* normals = nullptr;
        const float* texcoords = nullptr;
        const unsigned char* joints_u8 = nullptr;
        const unsigned short* joints_u16 = nullptr;
        const float* weights = nullptr;
        bool joints_are_u16 = false;

        auto norm_it = primitive.attributes.find("NORMAL");
        if (norm_it != primitive.attributes.end())
            normals = getAccessorData<float>(model, norm_it->second, normal_count);

        auto tex_it = primitive.attributes.find("TEXCOORD_0");
        if (tex_it != primitive.attributes.end())
            texcoords = getAccessorData<float>(model, tex_it->second, texcoord_count);

        auto joints_it = primitive.attributes.find("JOINTS_0");
        if (joints_it != primitive.attributes.end())
        {
            const auto& accessor = model.accessors[joints_it->second];
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                joints_u16 = getAccessorData<unsigned short>(model, joints_it->second, joints_count);
                joints_are_u16 = true;
            }
            else
            {
                joints_u8 = getAccessorData<unsigned char>(model, joints_it->second, joints_count);
            }
        }

        auto weights_it = primitive.attributes.find("WEIGHTS_0");
        if (weights_it != primitive.attributes.end())
            weights = getAccessorData<float>(model, weights_it->second, weights_count);

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
    }

    LOG_ENGINE_INFO("Loaded {} skinned vertices from mesh '{}'",
        out_vertices.size(), gltf_mesh.name);

    return !out_vertices.empty();
}

bool load(const tinygltf::Model& model, GltfAnimationData& out_data)
{
    out_data = {};

    if (model.skins.empty())
    {
        out_data.has_animation = false;
        return true; // Not an error, just no animation data
    }

    // Load skeleton from first skin
    out_data.skeleton = loadSkeleton(model, 0);
    if (!out_data.skeleton)
    {
        LOG_ENGINE_WARN("Failed to load skeleton from glTF");
        return false;
    }

    // Load animations
    out_data.clips = loadAnimations(model, *out_data.skeleton);

    // Load skinned vertices from the first mesh referenced by nodes with the skin
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
