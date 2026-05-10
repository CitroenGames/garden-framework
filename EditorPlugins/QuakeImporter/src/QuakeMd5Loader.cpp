// MD5 loader
// Paul R. All Rights Reserved,
// this is licensed under the license that garden uses, MIT.
// instead of attributing me, please attribute garden because this is a part of garden.

#include "QuakeMd5Loader.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace garden::assets {

    // ============================================================================
    // Utility Functions
    // ============================================================================

    namespace detail {

        /**
         * @brief Split a string by whitespace
         */
        static std::vector<std::string> splitString(const std::string& str) {
            std::vector<std::string> tokens;
            std::istringstream iss(str);
            std::string token;

            while (iss >> token) {
                tokens.push_back(token);
            }

            return tokens;
        }

        /**
         * @brief Remove leading/trailing whitespace
         */
        static std::string trim(const std::string& str) {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return "";

            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, last - first + 1);
        }

        /**
         * @brief Remove comments from line (everything after //)
         */
        static std::string removeComments(const std::string& line) {
            size_t pos = line.find("//");
            if (pos != std::string::npos) {
                return line.substr(0, pos);
            }
            return line;
        }

        /**
         * @brief Extract string between quotes
         */
        static std::string extractQuotedString(const std::string& line) {
            size_t first = line.find('\"');
            if (first == std::string::npos) return "";

            size_t last = line.find('\"', first + 1);
            if (last == std::string::npos) return "";

            return line.substr(first + 1, last - first - 1);
        }

        /**
         * @brief Read and preprocess file
         */
        static std::vector<std::string> readFile(const std::string& filename) {
            std::vector<std::string> lines;
            std::ifstream file(filename);

            if (!file.is_open()) {
                return lines;
            }

            std::string line;
            while (std::getline(file, line)) {
                line = removeComments(line);
                line = trim(line);

                if (!line.empty()) {
                    lines.push_back(line);
                }
            }

            file.close();
            return lines;
        }

    } // namespace detail

    // ============================================================================
    // MD5Model Implementation
    // ============================================================================

    bool MD5Model::load(const std::string& filename) {
        auto lines = detail::readFile(filename);
        if (lines.empty()) {
            return false;
        }

        int version = 0;
        int numJoints = 0;
        int numMeshes = 0;

        // Parse header
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];

            if (sscanf(line.c_str(), "MD5Version %d", &version) == 1) {
                if (version != MD5_VERSION) {
                    return false;
                }
            }
            else if (sscanf(line.c_str(), "numJoints %d", &numJoints) == 1) {
                baseSkeleton.resize(numJoints);
            }
            else if (sscanf(line.c_str(), "numMeshes %d", &numMeshes) == 1) {
                meshes.resize(numMeshes);
            }
            else if (line.find("joints {") != std::string::npos) {
                if (!parseJoints(line, i, lines)) {
                    return false;
                }
            }
            else if (line.find("mesh {") != std::string::npos) {
                if (!parseMesh(line, i, lines)) {
                    return false;
                }
            }
        }

        return !baseSkeleton.empty();
    }

    bool MD5Model::parseJoints(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        lineNum++; // Move past "joints {"

        for (size_t i = 0; i < baseSkeleton.size() && lineNum < lines.size(); ++i, ++lineNum) {
            const auto& jointLine = lines[lineNum];

            auto& joint = baseSkeleton[i];

            // Parse: "name" parent ( x y z ) ( qx qy qz )
            char name[64];
            float x, y, z, qx, qy, qz;
            int parent;

            if (sscanf(jointLine.c_str(), "\"%63[^\"]\" %d ( %f %f %f ) ( %f %f %f )",
                name, &parent, &x, &y, &z, &qx, &qy, &qz) == 8) {

                joint.name = name;
                joint.parent = parent;
                joint.pos = glm::vec3(x, y, z);
				joint.orient = quatFromMD5(qx, qy, qz);
            }
        }

        return true;
    }

    bool MD5Model::parseMesh(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        static int currentMesh = 0;
        if (currentMesh >= static_cast<int>(meshes.size())) {
            return false;
        }

        auto& mesh = meshes[currentMesh++];
        lineNum++; // Move past "mesh {"

        int numVerts = 0, numTris = 0, numWeights = 0;

        while (lineNum < lines.size()) {
            const auto& meshLine = lines[lineNum++];

            if (meshLine == "}") {
                break;
            }

            if (meshLine.find("shader ") != std::string::npos) {
                mesh.shader = detail::extractQuotedString(meshLine);
            }
            else if (sscanf(meshLine.c_str(), "numverts %d", &numVerts) == 1) {
                mesh.vertices.resize(numVerts);
            }
            else if (sscanf(meshLine.c_str(), "numtris %d", &numTris) == 1) {
                mesh.triangles.resize(numTris);
            }
            else if (sscanf(meshLine.c_str(), "numweights %d", &numWeights) == 1) {
                mesh.weights.resize(numWeights);
            }
            else {
                // Parse vertex data
                unsigned int vertIndex;
                float s, t;
                unsigned int start, count;

                if (sscanf(meshLine.c_str(), "vert %u ( %f %f ) %u %u",
                    &vertIndex, &s, &t, &start, &count) == 5) {

                    if (vertIndex < mesh.vertices.size()) {
                        mesh.vertices[vertIndex].texCoord = glm::vec2(s, t);
                        mesh.vertices[vertIndex].weightStart = start;
                        mesh.vertices[vertIndex].weightCount = count;
                    }
                }

                // Parse triangle data
                unsigned int triIndex, idx0, idx1, idx2;
                if (sscanf(meshLine.c_str(), "tri %u %u %u %u",
                    &triIndex, &idx0, &idx1, &idx2) == 4) {

                    if (triIndex < mesh.triangles.size()) {
                        mesh.triangles[triIndex].indices[0] = idx0;
                        mesh.triangles[triIndex].indices[1] = idx1;
                        mesh.triangles[triIndex].indices[2] = idx2;
                    }
                }

                // Parse weight data
                unsigned int weightIndex, joint;
                float bias, wx, wy, wz;

                if (sscanf(meshLine.c_str(), "weight %u %u %f ( %f %f %f )",
                    &weightIndex, &joint, &bias, &wx, &wy, &wz) == 7) {

                    if (weightIndex < mesh.weights.size()) {
                        mesh.weights[weightIndex].joint = joint;
                        mesh.weights[weightIndex].bias = bias;
                        mesh.weights[weightIndex].pos = glm::vec3(wx, wy, wz);
                    }
                }
            }
        }

        lineNum--; // Back up one line since the outer loop will increment
        return true;
    }

    void MD5Model::unload() {
        baseSkeleton.clear();
        meshes.clear();
    }

    // ============================================================================
    // MD5Animation Implementation
    // ============================================================================

    bool MD5Animation::load(const std::string& filename) {
        auto lines = detail::readFile(filename);
        if (lines.empty()) {
            return false;
        }

        int version = 0;

        // Parse header
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];

            if (sscanf(line.c_str(), "MD5Version %d", &version) == 1) {
                if (version != MD5_VERSION) {
                    return false;
                }
            }
            else if (sscanf(line.c_str(), "numFrames %u", &frameCount) == 1) {
                skeletonFrames.resize(frameCount);
                boundingBoxes.resize(frameCount);
            }
            else if (sscanf(line.c_str(), "numJoints %u", &jointCount) == 1) {
                for (auto& frame : skeletonFrames) {
                    frame.resize(jointCount);
                }
            }
            else if (sscanf(line.c_str(), "frameRate %u", &frameRate) == 1) {
                // Frame rate parsed
            }
            else if (line.find("hierarchy {") != std::string::npos) {
                if (!parseHierarchy(line, i, lines)) {
                    return false;
                }
            }
            else if (line.find("bounds {") != std::string::npos) {
                if (!parseBounds(line, i, lines)) {
                    return false;
                }
            }
            else if (line.find("baseframe {") != std::string::npos) {
                if (!parseBaseFrame(line, i, lines)) {
                    return false;
                }
            }
            else if (sscanf(line.c_str(), "frame %u", &i) == 1) {
                // Will be handled in parseFrame
                if (!parseFrame(line, i, lines)) {
                    return false;
                }
            }
        }

        return frameCount > 0 && jointCount > 0;
    }

    bool MD5Animation::parseHierarchy(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        lineNum++; // Move past "hierarchy {"
        jointInfos.resize(jointCount);

        for (size_t i = 0; i < jointCount && lineNum < lines.size(); ++i, ++lineNum) {
            const auto& hierLine = lines[lineNum];

            if (hierLine == "}") {
                return true;
            }

            // Parse: "name" parent flags startIndex
            char name[64];
            int parent, flags, startIndex;

            if (sscanf(hierLine.c_str(), "\"%63[^\"]\" %d %d %d",
                name, &parent, &flags, &startIndex) == 4) {

                jointInfos[i].name = name;
                jointInfos[i].parent = parent;
                jointInfos[i].flags = static_cast<uint32_t>(flags);
                jointInfos[i].startIndex = static_cast<uint32_t>(startIndex);
            }
        }

        return true;
    }

    bool MD5Animation::parseBounds(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        lineNum++; // Move past "bounds {"

        for (size_t i = 0; i < frameCount && lineNum < lines.size(); ++i, ++lineNum) {
            const auto& boundLine = lines[lineNum];

            if (boundLine == "}") {
                return true;
            }

            // Parse: ( min.x min.y min.z ) ( max.x max.y max.z )
            float minX, minY, minZ, maxX, maxY, maxZ;

            if (sscanf(boundLine.c_str(), "( %f %f %f ) ( %f %f %f )",
                &minX, &minY, &minZ, &maxX, &maxY, &maxZ) == 6) {

                boundingBoxes[i].min = glm::vec3(minX, minY, minZ);
                boundingBoxes[i].max = glm::vec3(maxX, maxY, maxZ);
            }
        }

        return true;
    }

    bool MD5Animation::parseBaseFrame(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        lineNum++; // Move past "baseframe {"
        baseFrame.resize(jointCount);

        for (size_t i = 0; i < jointCount && lineNum < lines.size(); ++i, ++lineNum) {
            const auto& baseLine = lines[lineNum];

            if (baseLine == "}") {
                return true;
            }

            // Parse: ( pos.x pos.y pos.z ) ( qx qy qz )
            float x, y, z, qx, qy, qz;

            if (sscanf(baseLine.c_str(), "( %f %f %f ) ( %f %f %f )",
                &x, &y, &z, &qx, &qy, &qz) == 6) {

                baseFrame[i].pos = glm::vec3(x, y, z);
				baseFrame[i].orient = quatFromMD5(qx, qy, qz);
            }
        }

        return true;
    }

    bool MD5Animation::parseFrame(const std::string& line, size_t& lineNum,
        const std::vector<std::string>& lines) {
        unsigned int frameIndex;

        if (sscanf(line.c_str(), "frame %u", &frameIndex) != 1) {
            return false;
        }

        if (frameIndex >= frameCount) {
            return false;
        }

        lineNum++; // Move past "frame X {"

        std::vector<float> frameData;

        // Read frame data until we hit '}'
        while (lineNum < lines.size()) {
            const auto& dataLine = lines[lineNum++];

            if (dataLine == "}") {
                break;
            }

            // Parse whitespace-separated floats
            std::istringstream iss(dataLine);
            float value;
            while (iss >> value) {
                frameData.push_back(value);
            }
        }

        // Build skeleton from frame data
        buildFrameSkeleton(frameIndex, frameData);

        return true;
    }

    void MD5Animation::buildFrameSkeleton(uint32_t frameIndex,
        const std::vector<float>& frameData) {
        auto& frame = skeletonFrames[frameIndex];

        for (size_t i = 0; i < jointCount; ++i) {
            const auto& baseJoint = baseFrame[i];
            const auto& info = jointInfos[i];

            // Start with base frame
            glm::vec3 animatedPos = baseJoint.pos;
            glm::quat animatedOrient = baseJoint.orient;

            uint32_t j = 0;

            // Replace components based on flags
            if (info.flags & 1) { // Tx
                if (info.startIndex + j < frameData.size()) {
                    animatedPos.x = frameData[info.startIndex + j];
                }
                ++j;
            }

            if (info.flags & 2) { // Ty
                if (info.startIndex + j < frameData.size()) {
                    animatedPos.y = frameData[info.startIndex + j];
                }
                ++j;
            }

            if (info.flags & 4) { // Tz
                if (info.startIndex + j < frameData.size()) {
                    animatedPos.z = frameData[info.startIndex + j];
                }
                ++j;
            }

            if (info.flags & 8) { // Qx
                if (info.startIndex + j < frameData.size()) {
                    animatedOrient.x = frameData[info.startIndex + j];
                }
                ++j;
            }

            if (info.flags & 16) { // Qy
                if (info.startIndex + j < frameData.size()) {
                    animatedOrient.y = frameData[info.startIndex + j];
                }
                ++j;
            }

            if (info.flags & 32) { // Qz
                if (info.startIndex + j < frameData.size()) {
                    animatedOrient.z = frameData[info.startIndex + j];
                }
                ++j;
            }

            // Ensure quaternion is normalized
			animatedOrient = quatFromMD5(animatedOrient.x, animatedOrient.y, animatedOrient.z);

            // Build final joint in object space
            auto& joint = frame[i];
            joint.name = info.name;
            joint.parent = info.parent;

            if (info.parent < 0) {
                // Root joint
                joint.pos = animatedPos;
                joint.orient = animatedOrient;
            }
            else {
                // Has parent - transform to object space
                const auto& parentJoint = frame[info.parent];

                // Rotate position by parent orientation
                glm::vec3 rotatedPos = parentJoint.orient * animatedPos;

                // Add to parent position
                joint.pos = parentJoint.pos + rotatedPos;

                // Concatenate rotations and normalize
                joint.orient = parentJoint.orient * animatedOrient;
                joint.orient = glm::normalize(joint.orient);
            }
        }
    }

    bool MD5Animation::isCompatibleWith(const MD5Model& model) const {
        if (model.getJointCount() != jointCount) {
            return false;
        }

        // Check against first frame
        if (skeletonFrames.empty()) {
            return false;
        }

        const auto& modelSkel = model.getBaseSkeleton();
        const auto& animSkel = skeletonFrames[0];

        for (size_t i = 0; i < jointCount; ++i) {
            // Check parent indices match
            if (modelSkel[i].parent != animSkel[i].parent) {
                return false;
            }

            // Check names match
            if (modelSkel[i].name != animSkel[i].name) {
                return false;
            }
        }

        return true;
    }

    void MD5Animation::unload() {
        frameCount = 0;
        jointCount = 0;
        skeletonFrames.clear();
        boundingBoxes.clear();
        jointInfos.clear();
        baseFrame.clear();
    }

} // namespace garden::assets