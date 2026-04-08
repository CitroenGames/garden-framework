#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

struct BonePose
{
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity quaternion
    glm::vec3 scale{1.0f};

    // Compose into a 4x4 matrix: T * R * S
    glm::mat4 toMatrix() const;

    // Identity pose (no transform)
    static BonePose identity();

    // Interpolate between two poses (mix for translation/scale, slerp for rotation)
    static BonePose lerp(const BonePose& a, const BonePose& b, float t);

    // Apply an additive delta on top of a base pose with a weight
    // delta is expected to be relative to a reference pose (subtract reference first)
    static BonePose additive(const BonePose& base, const BonePose& delta, float weight);
};

class Pose
{
public:
    Pose() = default;
    explicit Pose(int bone_count);

    void resize(int bone_count);
    int getBoneCount() const { return static_cast<int>(bones.size()); }

    BonePose& operator[](int index) { return bones[index]; }
    const BonePose& operator[](int index) const { return bones[index]; }

    // Convert all bone poses to a matrix array for GPU upload
    void toMatrices(std::vector<glm::mat4>& out) const;

    // Build from a matrix array (uses matrix decomposition)
    void fromMatrices(const std::vector<glm::mat4>& matrices);

    // Blend two poses using quaternion slerp + linear interpolation
    static Pose blend(const Pose& a, const Pose& b, float factor);

    // Additive blend: result = base + (additive - reference) * weight
    static Pose additiveBlend(const Pose& base, const Pose& additive,
                              const Pose& reference, float weight);

    // Masked blend: per-bone weights modulate the blend factor
    static Pose maskedBlend(const Pose& a, const Pose& b, float factor,
                            const std::vector<float>& bone_weights);

private:
    std::vector<BonePose> bones;
};
