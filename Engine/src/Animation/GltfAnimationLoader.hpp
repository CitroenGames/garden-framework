#pragma once

#include "Skeleton.hpp"
#include "AnimationClip.hpp"
#include "Utils/SkinnedVertex.hpp"
#include <string>
#include <vector>
#include <memory>

// Forward declare tinygltf types to avoid including the heavy header here
namespace tinygltf { class Model; }

struct GltfAnimationData
{
    std::shared_ptr<Skeleton> skeleton;
    std::vector<std::shared_ptr<AnimationClip>> clips;
    std::vector<skinned_vertex> skinned_vertices;
    bool has_animation = false;
};

namespace GltfAnimationLoader
{
    // Load skeleton, animations, and skinned vertex data from a glTF model
    // The tinygltf::Model must already be loaded
    bool load(const tinygltf::Model& model, GltfAnimationData& out_data);

    // Load only skeleton (joints + inverse bind matrices)
    std::shared_ptr<Skeleton> loadSkeleton(const tinygltf::Model& model, int skin_index);

    // Load animation clips
    std::vector<std::shared_ptr<AnimationClip>> loadAnimations(
        const tinygltf::Model& model,
        const Skeleton& skeleton);

    // Extract skinned vertex data from the first mesh with skin weights
    bool loadSkinnedVertices(const tinygltf::Model& model, int mesh_index,
                             std::vector<skinned_vertex>& out_vertices);
}
