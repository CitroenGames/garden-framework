#pragma once
/*
Implementation of a Doom3/Quake4 (i.e. id Tech 4) MD5 model and animation loader.
I was very careful as not to basically just, (fucking) copy paste GPL code because
we dont want that in an MIT project, as that would essentially destroy the whole purpose
of having a more liberal license if it gets overriden during distribution.

Specifically adapted for Garden.
*/

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vector>
#include <string>
#include <memory>
#include <array>
#include <cmath>


namespace garden::assets {

    // ============================================================================
    // Constants
    // ============================================================================

    constexpr int MD5_VERSION = 10;
    constexpr int MD5_MAX_JOINTS = 128;

    // ============================================================================
    // MD5 quaternion helper
    // ============================================================================

    inline glm::quat quatFromMD5(float x, float y, float z)
    {
        float t = 1.0f - (x * x) - (y * y) - (z * z);

        float w = (t < 0.0f) ? 0.0f : -std::sqrt(t);

        return glm::normalize(glm::quat(w, x, y, z));
    }

    // ============================================================================
    // Core data structures
    // ============================================================================

    struct Joint {
        std::string name;
        int parent;
        glm::vec3 pos;
        glm::quat orient;

        Joint()
            : parent(-1),
            pos(0.0f),
            orient(1.0f, 0.0f, 0.0f, 0.0f) {}
    };

    struct Vertex {
        glm::vec2 texCoord;
        uint32_t weightStart;
        uint32_t weightCount;

        Vertex()
            : texCoord(0.0f),
            weightStart(0),
            weightCount(0) {}
    };

    struct Triangle {
        std::array<uint32_t, 3> indices;
        Triangle() : indices{ 0, 0, 0 } {}
    };

    struct Weight {
        uint32_t joint;
        float bias;
        glm::vec3 pos;

        Weight()
            : joint(0),
            bias(0.0f),
            pos(0.0f) {}
    };

    struct BoundingBox {
        glm::vec3 min;
        glm::vec3 max;

        BoundingBox()
            : min(0.0f),
            max(0.0f) {}
    };

    // ============================================================================
    // Mesh
    // ============================================================================

    class Mesh {
    public:
        std::string shader;
        std::vector<Vertex> vertices;
        std::vector<Triangle> triangles;
        std::vector<Weight> weights;

        std::vector<glm::vec3> computeVertexPositions(
            const std::vector<Joint>& skeleton) const;
    };

    // ============================================================================
    // MD5 Model
    // ============================================================================

    class MD5Model {
    public:
        bool load(const std::string& filename);
        void unload();

        const std::vector<Joint>& getBaseSkeleton() const { return baseSkeleton; }
        size_t getJointCount() const { return baseSkeleton.size(); }
        const std::vector<Mesh>& getMeshes() const { return meshes; }

    private:
        std::vector<Joint> baseSkeleton;
        std::vector<Mesh> meshes;

        bool parseJoints(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);

        bool parseMesh(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);
    };

    // ============================================================================
    // Animation structures
    // ============================================================================

    struct JointInfo {
        std::string name;
        int parent;
        uint32_t flags;
        uint32_t startIndex;

        JointInfo()
            : parent(-1),
            flags(0),
            startIndex(0) {}
    };

    // ============================================================================
    // MD5 Animation
    // ============================================================================

    class MD5Animation {
    public:
        bool load(const std::string& filename);
        void unload();

        bool isCompatibleWith(const MD5Model& model) const;

        uint32_t getFrameCount() const { return frameCount; }
        uint32_t getFrameRate() const { return frameRate; }

        float getDuration() const {
            return frameRate ? float(frameCount) / float(frameRate) : 0.0f;
        }

        uint32_t getJointCount() const { return jointCount; }

        const std::vector<Joint>* getFrame(uint32_t frameIndex) const {
            return frameIndex < skeletonFrames.size()
                ? &skeletonFrames[frameIndex]
                : nullptr;
        }

        const BoundingBox* getBoundingBox(uint32_t frameIndex) const {
            return frameIndex < boundingBoxes.size()
                ? &boundingBoxes[frameIndex]
                : nullptr;
        }

    private:
        uint32_t frameCount = 0;
        uint32_t jointCount = 0;
        uint32_t frameRate = 24;

        std::vector<std::vector<Joint>> skeletonFrames;
        std::vector<BoundingBox> boundingBoxes;
        std::vector<JointInfo> jointInfos;
        std::vector<Joint> baseFrame;

        bool parseHierarchy(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);

        bool parseBounds(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);

        bool parseBaseFrame(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);

        bool parseFrame(const std::string& line, size_t& lineNum,
            const std::vector<std::string>& lines);

        void buildFrameSkeleton(uint32_t frameIndex,
            const std::vector<float>& frameData);
    };

    // ============================================================================
    // Animation state
    // ============================================================================

    struct AnimationState {
        uint32_t currentFrame = 0;
        uint32_t nextFrame = 1;
        double elapsedTime = 0.0;
        double maxFrameTime = 1.0 / 24.0;

        void update(double deltaTime, const MD5Animation& animation);

        float getInterpolationFactor() const {
            return maxFrameTime > 0.0
                ? float(elapsedTime / maxFrameTime)
                : 0.0f;
        }
    };

    // ============================================================================
    // Interpolation
    // ============================================================================

    class SkeletonInterpolator {
    public:
        static std::vector<Joint> interpolate(
            const std::vector<Joint>& skelA,
            const std::vector<Joint>& skelB,
            float t);
    };

    // ============================================================================
    // Mesh implementation
    // ============================================================================

    inline std::vector<glm::vec3> Mesh::computeVertexPositions(
        const std::vector<Joint>& skeleton) const {

        std::vector<glm::vec3> positions;
        positions.reserve(vertices.size());

        for (const auto& vertex : vertices) {
            glm::vec3 finalPos(0.0f);

            for (uint32_t i = 0; i < vertex.weightCount; ++i) {
                const auto& weight = weights[vertex.weightStart + i];
                const auto& joint = skeleton[weight.joint];

                glm::vec3 rotated = joint.orient * weight.pos;

                finalPos += (joint.pos + rotated) * weight.bias;
            }

            positions.push_back(finalPos);
        }

        return positions;
    }

    // ============================================================================
    // Animation state update
    // ============================================================================

    inline void AnimationState::update(double deltaTime,
        const MD5Animation& animation) {
        if (!animation.getFrameRate()) return;

        maxFrameTime = 1.0 / animation.getFrameRate();
        elapsedTime += deltaTime;

        while (elapsedTime >= maxFrameTime) {
            currentFrame++;
            nextFrame++;
            elapsedTime -= maxFrameTime;

            if (currentFrame >= animation.getFrameCount())
                currentFrame = 0;

            if (nextFrame >= animation.getFrameCount())
                nextFrame = 0;
        }
    }

    // ============================================================================
    // Skeleton interpolation
    // ============================================================================

    inline std::vector<Joint> SkeletonInterpolator::interpolate(
        const std::vector<Joint>& skelA,
        const std::vector<Joint>& skelB,
        float t) {

        std::vector<Joint> result;
        result.reserve(skelA.size());

        for (size_t i = 0; i < skelA.size(); ++i) {
            Joint j;
            j.name = skelA[i].name;
            j.parent = skelA[i].parent;

            j.pos = glm::mix(skelA[i].pos, skelB[i].pos, t);
            glm::slerp(skelA[i].orient, skelB[i].orient, t);

            result.push_back(j);
        }

        return result;
    }

} // namespace garden::assets