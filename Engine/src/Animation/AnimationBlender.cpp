#include "AnimationBlender.hpp"
#include <algorithm>

// ---- Internal helpers ----

void AnimationBlender::ensureBaseLayer()
{
    if (layers.empty())
    {
        layers.emplace_back();
    }
}

// ---- Legacy API (delegates to layer 0) ----

void AnimationBlender::play(std::shared_ptr<AnimationClip> clip, float blend_time)
{
    ensureBaseLayer();
    layers[0].play(std::move(clip), blend_time);
}

void AnimationBlender::stop()
{
    if (!layers.empty())
    {
        layers[0].stop();
    }
}

void AnimationBlender::setSpeed(float speed)
{
    ensureBaseLayer();
    layers[0].playback_speed = speed;
}

float AnimationBlender::getSpeed() const
{
    return layers.empty() ? 1.0f : layers[0].playback_speed;
}

void AnimationBlender::setLooping(bool loop)
{
    ensureBaseLayer();
    layers[0].looping = loop;
}

bool AnimationBlender::isLooping() const
{
    return layers.empty() ? true : layers[0].looping;
}

bool AnimationBlender::isPlaying() const
{
    return !layers.empty() && layers[0].playing;
}

bool AnimationBlender::isBlending() const
{
    return !layers.empty() && layers[0].blend_from_clip != nullptr && layers[0].blend_factor < 1.0f;
}

float AnimationBlender::getPlaybackTime() const
{
    return layers.empty() ? 0.0f : layers[0].playback_time;
}

const AnimationClip* AnimationBlender::getCurrentClip() const
{
    return layers.empty() ? nullptr : layers[0].clip.get();
}

void AnimationBlender::update(float dt, int bone_count, std::vector<glm::mat4>& out_local_poses)
{
    Pose pose(bone_count);
    updatePose(dt, bone_count, pose);
    pose.toMatrices(out_local_poses);
}

// ---- New Pose-based API ----

void AnimationBlender::updatePose(float dt, int bone_count, Pose& out_pose)
{
    out_pose.resize(bone_count);

    // Initialize to identity
    for (int i = 0; i < bone_count; i++)
    {
        out_pose[i] = BonePose::identity();
    }

    if (layers.empty()) return;

    // Evaluate layer 0 (base layer)
    Pose base_pose(bone_count);
    bool has_base = layers[0].update(dt, bone_count, base_pose);

    if (has_base)
    {
        out_pose = base_pose;
    }

    // Evaluate additional layers and combine
    for (int layer_idx = 1; layer_idx < static_cast<int>(layers.size()); layer_idx++)
    {
        auto& layer = layers[layer_idx];

        Pose layer_pose(bone_count);
        bool has_pose = layer.update(dt, bone_count, layer_pose);
        if (!has_pose) continue;

        const auto& mask_weights = layer.mask.getWeights();

        if (layer.mode == BlendMode::Override)
        {
            out_pose = Pose::maskedBlend(out_pose, layer_pose, layer.weight, mask_weights);
        }
        else // BlendMode::Additive
        {
            // Compute additive result
            Pose additive_result = Pose::additiveBlend(out_pose, layer_pose,
                                                        layer.reference_pose, layer.weight);

            // Apply mask: blend between current accumulated and additive result
            if (!mask_weights.empty())
            {
                out_pose = Pose::maskedBlend(out_pose, additive_result, 1.0f, mask_weights);
            }
            else
            {
                out_pose = additive_result;
            }
        }
    }
}

// ---- Layer management ----

int AnimationBlender::addLayer(BlendMode mode)
{
    ensureBaseLayer(); // make sure layer 0 exists
    AnimationLayer layer;
    layer.mode = mode;
    layers.push_back(std::move(layer));
    return static_cast<int>(layers.size()) - 1;
}

AnimationLayer& AnimationBlender::getLayer(int index)
{
    return layers[index];
}

const AnimationLayer& AnimationBlender::getLayer(int index) const
{
    return layers[index];
}

void AnimationBlender::removeLayer(int index)
{
    if (index > 0 && index < static_cast<int>(layers.size()))
    {
        layers.erase(layers.begin() + index);
    }
}
