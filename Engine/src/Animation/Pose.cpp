#include "Pose.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>

// ---- BonePose ----

glm::mat4 BonePose::toMatrix() const
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
    glm::mat4 r = glm::toMat4(rotation);
    glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
    return t * r * s;
}

BonePose BonePose::identity()
{
    BonePose p;
    p.translation = glm::vec3(0.0f);
    p.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    p.scale = glm::vec3(1.0f);
    return p;
}

BonePose BonePose::lerp(const BonePose& a, const BonePose& b, float t)
{
    BonePose result;
    result.translation = glm::mix(a.translation, b.translation, t);
    result.scale = glm::mix(a.scale, b.scale, t);

    // Ensure quaternions are in the same hemisphere before slerp
    glm::quat b_rot = b.rotation;
    if (glm::dot(a.rotation, b_rot) < 0.0f)
        b_rot = -b_rot;

    result.rotation = glm::slerp(a.rotation, b_rot, t);
    return result;
}

BonePose BonePose::additive(const BonePose& base, const BonePose& delta, float weight)
{
    BonePose result;
    result.translation = base.translation + delta.translation * weight;
    result.scale = base.scale + delta.scale * weight;

    // Slerp from identity toward delta rotation, then multiply onto base
    glm::quat identity_q(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat delta_rot = delta.rotation;
    if (glm::dot(identity_q, delta_rot) < 0.0f)
        delta_rot = -delta_rot;

    glm::quat weighted_delta = glm::slerp(identity_q, delta_rot, weight);
    result.rotation = weighted_delta * base.rotation;
    result.rotation = glm::normalize(result.rotation);
    return result;
}

// ---- Pose ----

Pose::Pose(int bone_count)
    : bones(bone_count)
{
}

void Pose::resize(int bone_count)
{
    bones.resize(bone_count);
}

void Pose::toMatrices(std::vector<glm::mat4>& out) const
{
    out.resize(bones.size());
    for (int i = 0; i < static_cast<int>(bones.size()); i++)
    {
        out[i] = bones[i].toMatrix();
    }
}

void Pose::fromMatrices(const std::vector<glm::mat4>& matrices)
{
    bones.resize(matrices.size());
    for (int i = 0; i < static_cast<int>(matrices.size()); i++)
    {
        glm::vec3 translation;
        glm::quat rotation;
        glm::vec3 scale;
        glm::vec3 skew;
        glm::vec4 perspective;

        if (glm::decompose(matrices[i], scale, rotation, translation, skew, perspective))
        {
            bones[i].translation = translation;
            bones[i].rotation = rotation;
            bones[i].scale = scale;
        }
        else
        {
            // Fallback to identity if decomposition fails
            bones[i] = BonePose::identity();
        }
    }
}

Pose Pose::blend(const Pose& a, const Pose& b, float factor)
{
    int count = std::min(a.getBoneCount(), b.getBoneCount());
    Pose result(count);

    for (int i = 0; i < count; i++)
    {
        result[i] = BonePose::lerp(a[i], b[i], factor);
    }
    return result;
}

Pose Pose::additiveBlend(const Pose& base, const Pose& additive,
                          const Pose& reference, float weight)
{
    int count = base.getBoneCount();
    Pose result(count);

    for (int i = 0; i < count; i++)
    {
        if (i < additive.getBoneCount() && i < reference.getBoneCount())
        {
            // Compute delta: additive - reference
            BonePose delta;
            delta.translation = additive[i].translation - reference[i].translation;
            delta.scale = additive[i].scale - reference[i].scale;
            // Delta rotation = additive * inverse(reference)
            delta.rotation = additive[i].rotation * glm::inverse(reference[i].rotation);

            result[i] = BonePose::additive(base[i], delta, weight);
        }
        else
        {
            result[i] = base[i];
        }
    }
    return result;
}

Pose Pose::maskedBlend(const Pose& a, const Pose& b, float factor,
                        const std::vector<float>& bone_weights)
{
    int count = std::min(a.getBoneCount(), b.getBoneCount());
    Pose result(count);

    for (int i = 0; i < count; i++)
    {
        float w = (i < static_cast<int>(bone_weights.size())) ? bone_weights[i] : 1.0f;
        float effective_factor = factor * w;
        result[i] = BonePose::lerp(a[i], b[i], effective_factor);
    }
    return result;
}
