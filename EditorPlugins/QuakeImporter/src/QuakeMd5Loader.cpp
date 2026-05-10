#include "QuakeMd5Loader.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace garden::assets {

namespace detail {

static std::string trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";

    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

static std::string removeComments(const std::string& line)
{
    size_t pos = line.find("//");
    if (pos != std::string::npos) {
        return line.substr(0, pos);
    }
    return line;
}

static std::string extractQuotedString(const std::string& line)
{
    size_t first = line.find('"');
    if (first == std::string::npos) return "";

    size_t last = line.find('"', first + 1);
    if (last == std::string::npos) return "";

    return line.substr(first + 1, last - first - 1);
}

static std::vector<std::string> readFile(const std::string& filename)
{
    std::vector<std::string> lines;
    std::ifstream file(filename);
    if (!file.is_open()) {
        return lines;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(removeComments(line));
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

static bool validParentIndex(int parent, size_t jointIndex)
{
    return parent == -1 || (parent >= 0 && static_cast<size_t>(parent) < jointIndex);
}

} // namespace detail

bool MD5Model::load(const std::string& filename)
{
    unload();

    auto lines = detail::readFile(filename);
    if (lines.empty()) {
        return false;
    }

    bool sawVersion = false;
    bool sawJoints = false;
    bool sawMeshes = false;
    int version = 0;
    int numJoints = 0;
    int numMeshes = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];

        if (std::sscanf(line.c_str(), "MD5Version %d", &version) == 1) {
            sawVersion = true;
            if (version != MD5_VERSION) {
                return false;
            }
        } else if (std::sscanf(line.c_str(), "numJoints %d", &numJoints) == 1) {
            if (numJoints <= 0 || numJoints > MD5_MAX_JOINTS) {
                return false;
            }
            baseSkeleton.resize(static_cast<size_t>(numJoints));
        } else if (std::sscanf(line.c_str(), "numMeshes %d", &numMeshes) == 1) {
            if (numMeshes <= 0) {
                return false;
            }
            meshes.resize(static_cast<size_t>(numMeshes));
        } else if (line == "joints {") {
            if (baseSkeleton.empty() || !parseJoints(line, i, lines)) {
                return false;
            }
            sawJoints = true;
        } else if (line == "mesh {") {
            if (meshes.empty() || !parseMesh(line, i, lines)) {
                return false;
            }
            sawMeshes = true;
        }
    }

    return sawVersion && sawJoints && sawMeshes && meshParseIndex == meshes.size();
}

bool MD5Model::parseJoints(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    (void)line;
    ++lineNum;

    for (size_t i = 0; i < baseSkeleton.size(); ++i, ++lineNum) {
        if (lineNum >= lines.size() || lines[lineNum] == "}") {
            return false;
        }

        char name[64] = {};
        float x = 0.0f, y = 0.0f, z = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        int parent = -1;

        if (std::sscanf(lines[lineNum].c_str(), "\"%63[^\"]\" %d ( %f %f %f ) ( %f %f %f )",
            name, &parent, &x, &y, &z, &qx, &qy, &qz) != 8) {
            return false;
        }

        if (!detail::validParentIndex(parent, i)) {
            return false;
        }

        auto& joint = baseSkeleton[i];
        joint.name = name;
        joint.parent = parent;
        joint.pos = glm::vec3(x, y, z);
        joint.orient = quatFromMD5(qx, qy, qz);
    }

    return lineNum < lines.size() && lines[lineNum] == "}";
}

bool MD5Model::parseMesh(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    (void)line;
    if (meshParseIndex >= meshes.size()) {
        return false;
    }

    auto& mesh = meshes[meshParseIndex++];
    ++lineNum;

    while (lineNum < lines.size()) {
        const auto& meshLine = lines[lineNum];
        if (meshLine == "}") {
            break;
        }

        if (meshLine.rfind("shader ", 0) == 0) {
            mesh.shader = detail::extractQuotedString(meshLine);
        } else {
            int count = 0;
            if (std::sscanf(meshLine.c_str(), "numverts %d", &count) == 1) {
                if (count < 0) return false;
                mesh.vertices.resize(static_cast<size_t>(count));
            } else if (std::sscanf(meshLine.c_str(), "numtris %d", &count) == 1) {
                if (count < 0) return false;
                mesh.triangles.resize(static_cast<size_t>(count));
            } else if (std::sscanf(meshLine.c_str(), "numweights %d", &count) == 1) {
                if (count < 0) return false;
                mesh.weights.resize(static_cast<size_t>(count));
            } else if (meshLine.rfind("vert ", 0) == 0) {
                unsigned int vertIndex = 0, start = 0, weightCount = 0;
                float s = 0.0f, t = 0.0f;

                if (std::sscanf(meshLine.c_str(), "vert %u ( %f %f ) %u %u",
                    &vertIndex, &s, &t, &start, &weightCount) != 5 ||
                    vertIndex >= mesh.vertices.size()) {
                    return false;
                }

                mesh.vertices[vertIndex].texCoord = glm::vec2(s, t);
                mesh.vertices[vertIndex].weightStart = start;
                mesh.vertices[vertIndex].weightCount = weightCount;
            } else if (meshLine.rfind("tri ", 0) == 0) {
                unsigned int triIndex = 0, idx0 = 0, idx1 = 0, idx2 = 0;

                if (std::sscanf(meshLine.c_str(), "tri %u %u %u %u",
                    &triIndex, &idx0, &idx1, &idx2) != 4 ||
                    triIndex >= mesh.triangles.size()) {
                    return false;
                }

                mesh.triangles[triIndex].indices[0] = idx0;
                mesh.triangles[triIndex].indices[1] = idx1;
                mesh.triangles[triIndex].indices[2] = idx2;
            } else if (meshLine.rfind("weight ", 0) == 0) {
                unsigned int weightIndex = 0, joint = 0;
                float bias = 0.0f, wx = 0.0f, wy = 0.0f, wz = 0.0f;

                if (std::sscanf(meshLine.c_str(), "weight %u %u %f ( %f %f %f )",
                    &weightIndex, &joint, &bias, &wx, &wy, &wz) != 7 ||
                    weightIndex >= mesh.weights.size() ||
                    joint >= baseSkeleton.size()) {
                    return false;
                }

                mesh.weights[weightIndex].joint = joint;
                mesh.weights[weightIndex].bias = bias;
                mesh.weights[weightIndex].pos = glm::vec3(wx, wy, wz);
            }
        }

        ++lineNum;
    }

    if (lineNum >= lines.size() || lines[lineNum] != "}") {
        return false;
    }

    for (const auto& vertex : mesh.vertices) {
        if (vertex.weightCount == 0 ||
            vertex.weightStart > mesh.weights.size() ||
            vertex.weightCount > mesh.weights.size() - vertex.weightStart) {
            return false;
        }
    }

    for (const auto& tri : mesh.triangles) {
        if (tri.indices[0] >= mesh.vertices.size() ||
            tri.indices[1] >= mesh.vertices.size() ||
            tri.indices[2] >= mesh.vertices.size()) {
            return false;
        }
    }

    return true;
}

void MD5Model::unload()
{
    baseSkeleton.clear();
    meshes.clear();
    meshParseIndex = 0;
}

bool MD5Animation::load(const std::string& filename)
{
    unload();

    auto lines = detail::readFile(filename);
    if (lines.empty()) {
        return false;
    }

    bool sawVersion = false;
    bool sawHierarchy = false;
    bool sawBounds = false;
    bool sawBaseFrame = false;
    int version = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];

        if (std::sscanf(line.c_str(), "MD5Version %d", &version) == 1) {
            sawVersion = true;
            if (version != MD5_VERSION) {
                return false;
            }
        } else if (std::sscanf(line.c_str(), "numFrames %u", &frameCount) == 1) {
            if (frameCount == 0) return false;
            skeletonFrames.resize(frameCount);
            boundingBoxes.resize(frameCount);
            if (jointCount > 0) {
                for (auto& frame : skeletonFrames) frame.resize(jointCount);
            }
        } else if (std::sscanf(line.c_str(), "numJoints %u", &jointCount) == 1) {
            if (jointCount == 0 || jointCount > MD5_MAX_JOINTS) return false;
            for (auto& frame : skeletonFrames) frame.resize(jointCount);
        } else if (std::sscanf(line.c_str(), "frameRate %u", &frameRate) == 1) {
            if (frameRate == 0) return false;
        } else if (line == "hierarchy {") {
            if (jointCount == 0 || !parseHierarchy(line, i, lines)) return false;
            sawHierarchy = true;
        } else if (line == "bounds {") {
            if (frameCount == 0 || !parseBounds(line, i, lines)) return false;
            sawBounds = true;
        } else if (line == "baseframe {") {
            if (jointCount == 0 || !parseBaseFrame(line, i, lines)) return false;
            sawBaseFrame = true;
        } else if (line.rfind("frame ", 0) == 0) {
            if (!parseFrame(line, i, lines)) return false;
        }
    }

    return sawVersion && sawHierarchy && sawBounds && sawBaseFrame &&
        frameCount > 0 && jointCount > 0 && skeletonFrames.size() == frameCount;
}

bool MD5Animation::parseHierarchy(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    (void)line;
    ++lineNum;
    jointInfos.resize(jointCount);

    for (size_t i = 0; i < jointCount; ++i, ++lineNum) {
        if (lineNum >= lines.size() || lines[lineNum] == "}") {
            return false;
        }

        char name[64] = {};
        int parent = -1, flags = 0, startIndex = 0;

        if (std::sscanf(lines[lineNum].c_str(), "\"%63[^\"]\" %d %d %d",
            name, &parent, &flags, &startIndex) != 4 ||
            flags < 0 || (flags & ~63) != 0 || startIndex < 0 ||
            !detail::validParentIndex(parent, i)) {
            return false;
        }

        jointInfos[i].name = name;
        jointInfos[i].parent = parent;
        jointInfos[i].flags = static_cast<uint32_t>(flags);
        jointInfos[i].startIndex = static_cast<uint32_t>(startIndex);
    }

    return lineNum < lines.size() && lines[lineNum] == "}";
}

bool MD5Animation::parseBounds(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    (void)line;
    ++lineNum;

    for (size_t i = 0; i < frameCount; ++i, ++lineNum) {
        if (lineNum >= lines.size() || lines[lineNum] == "}") {
            return false;
        }

        float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
        float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;

        if (std::sscanf(lines[lineNum].c_str(), "( %f %f %f ) ( %f %f %f )",
            &minX, &minY, &minZ, &maxX, &maxY, &maxZ) != 6) {
            return false;
        }

        boundingBoxes[i].min = glm::vec3(minX, minY, minZ);
        boundingBoxes[i].max = glm::vec3(maxX, maxY, maxZ);
    }

    return lineNum < lines.size() && lines[lineNum] == "}";
}

bool MD5Animation::parseBaseFrame(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    (void)line;
    ++lineNum;
    baseFrame.resize(jointCount);

    for (size_t i = 0; i < jointCount; ++i, ++lineNum) {
        if (lineNum >= lines.size() || lines[lineNum] == "}") {
            return false;
        }

        float x = 0.0f, y = 0.0f, z = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;

        if (std::sscanf(lines[lineNum].c_str(), "( %f %f %f ) ( %f %f %f )",
            &x, &y, &z, &qx, &qy, &qz) != 6) {
            return false;
        }

        baseFrame[i].pos = glm::vec3(x, y, z);
        baseFrame[i].orient = quatFromMD5(qx, qy, qz);
    }

    return lineNum < lines.size() && lines[lineNum] == "}";
}

bool MD5Animation::parseFrame(const std::string& line, size_t& lineNum,
    const std::vector<std::string>& lines)
{
    unsigned int frameIndex = 0;
    if (std::sscanf(line.c_str(), "frame %u", &frameIndex) != 1 ||
        frameIndex >= frameCount ||
        jointInfos.size() != jointCount ||
        baseFrame.size() != jointCount ||
        skeletonFrames.size() != frameCount) {
        return false;
    }

    ++lineNum;

    std::vector<float> frameData;
    while (lineNum < lines.size()) {
        const auto& dataLine = lines[lineNum];
        if (dataLine == "}") {
            break;
        }

        std::istringstream iss(dataLine);
        float value = 0.0f;
        while (iss >> value) {
            frameData.push_back(value);
        }

        ++lineNum;
    }

    if (lineNum >= lines.size() || lines[lineNum] != "}") {
        return false;
    }

    return buildFrameSkeleton(frameIndex, frameData);
}

bool MD5Animation::buildFrameSkeleton(uint32_t frameIndex,
    const std::vector<float>& frameData)
{
    if (frameIndex >= skeletonFrames.size() ||
        jointInfos.size() != jointCount ||
        baseFrame.size() != jointCount) {
        return false;
    }

    auto& frame = skeletonFrames[frameIndex];

    for (size_t i = 0; i < jointCount; ++i) {
        const auto& baseJoint = baseFrame[i];
        const auto& info = jointInfos[i];

        glm::vec3 animatedPos = baseJoint.pos;
        glm::quat animatedOrient = baseJoint.orient;
        uint32_t j = 0;

        auto readComponent = [&](float& out) {
            const size_t index = static_cast<size_t>(info.startIndex + j);
            if (index >= frameData.size()) {
                return false;
            }
            out = frameData[index];
            ++j;
            return true;
        };

        if ((info.flags & 1) && !readComponent(animatedPos.x)) return false;
        if ((info.flags & 2) && !readComponent(animatedPos.y)) return false;
        if ((info.flags & 4) && !readComponent(animatedPos.z)) return false;
        if ((info.flags & 8) && !readComponent(animatedOrient.x)) return false;
        if ((info.flags & 16) && !readComponent(animatedOrient.y)) return false;
        if ((info.flags & 32) && !readComponent(animatedOrient.z)) return false;

        animatedOrient = quatFromMD5(animatedOrient.x, animatedOrient.y, animatedOrient.z);

        auto& joint = frame[i];
        joint.name = info.name;
        joint.parent = info.parent;

        if (info.parent < 0) {
            joint.pos = animatedPos;
            joint.orient = animatedOrient;
        } else {
            if (static_cast<size_t>(info.parent) >= i) {
                return false;
            }

            const auto& parentJoint = frame[static_cast<size_t>(info.parent)];
            joint.pos = parentJoint.pos + parentJoint.orient * animatedPos;
            joint.orient = glm::normalize(parentJoint.orient * animatedOrient);
        }
    }

    return true;
}

bool MD5Animation::isCompatibleWith(const MD5Model& model) const
{
    if (model.getJointCount() != jointCount || skeletonFrames.empty()) {
        return false;
    }

    const auto& modelSkel = model.getBaseSkeleton();
    const auto& animSkel = skeletonFrames[0];

    for (size_t i = 0; i < jointCount; ++i) {
        if (modelSkel[i].parent != animSkel[i].parent ||
            modelSkel[i].name != animSkel[i].name) {
            return false;
        }
    }

    return true;
}

void MD5Animation::unload()
{
    frameCount = 0;
    jointCount = 0;
    frameRate = 24;
    skeletonFrames.clear();
    boundingBoxes.clear();
    jointInfos.clear();
    baseFrame.clear();
}

} // namespace garden::assets
