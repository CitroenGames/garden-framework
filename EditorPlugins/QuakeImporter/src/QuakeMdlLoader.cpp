#include "QuakeMdlLoader.hpp"

#include "Assets/MeshAssetData.hpp"
#include "Graphics/IGPUMesh.hpp"
#include "Graphics/RenderAPI.hpp"
#include "QuakeMd5Loader.h"
#include "Utils/Log.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>

namespace QuakeImporter {

namespace {

static std::string lowerExtension(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return "";
    }

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

static void addTriangleNormal(Assets::MeshAssetData& meshData,
    uint32_t i0, uint32_t i1, uint32_t i2)
{
    auto& v0 = meshData.vertices[i0];
    auto& v1 = meshData.vertices[i1];
    auto& v2 = meshData.vertices[i2];

    glm::vec3 p0(v0.vx, v0.vy, v0.vz);
    glm::vec3 p1(v1.vx, v1.vy, v1.vz);
    glm::vec3 p2(v2.vx, v2.vy, v2.vz);

    glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
    const float len2 = glm::dot(normal, normal);
    if (len2 <= 0.0000001f) {
        return;
    }
    normal = glm::normalize(normal);

    v0.nx += normal.x; v0.ny += normal.y; v0.nz += normal.z;
    v1.nx += normal.x; v1.ny += normal.y; v1.nz += normal.z;
    v2.nx += normal.x; v2.ny += normal.y; v2.nz += normal.z;
}

static void normalizeNormals(Assets::MeshAssetData& meshData)
{
    for (auto& v : meshData.vertices) {
        glm::vec3 n(v.nx, v.ny, v.nz);
        const float len2 = glm::dot(n, n);
        if (len2 > 0.0000001f) {
            n = glm::normalize(n);
        } else {
            n = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        v.nx = n.x; v.ny = n.y; v.nz = n.z;
    }
}

static bool buildMeshData(const garden::assets::MD5Model& model,
    Assets::MeshAssetData& meshData,
    std::string& error)
{
    const auto& skeleton = model.getBaseSkeleton();
    if (skeleton.empty() || model.getMeshes().empty()) {
        error = "MD5 mesh has no skeleton or mesh data";
        return false;
    }

    meshData.use_indices = true;

    for (const auto& md5Mesh : model.getMeshes()) {
        const size_t baseVertex = meshData.vertices.size();
        const size_t indexStart = meshData.indices.size();

        std::vector<glm::vec3> positions = md5Mesh.computeVertexPositions(skeleton);
        if (positions.size() != md5Mesh.vertices.size()) {
            error = "MD5 mesh contains invalid weight or joint references";
            return false;
        }

        meshData.vertices.reserve(meshData.vertices.size() + positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
            const auto& src = md5Mesh.vertices[i];
            const auto& pos = positions[i];

            vertex dst{};
            dst.vx = pos.x;
            dst.vy = pos.y;
            dst.vz = pos.z;
            dst.u = src.texCoord.x;
            dst.v = src.texCoord.y;
            dst.tx = 1.0f;
            dst.ty = 0.0f;
            dst.tz = 0.0f;
            dst.tw = 1.0f;
            meshData.vertices.push_back(dst);
        }

        meshData.indices.reserve(meshData.indices.size() + md5Mesh.triangles.size() * 3);
        for (const auto& tri : md5Mesh.triangles) {
            for (uint32_t idx : tri.indices) {
                if (idx >= positions.size()) {
                    error = "MD5 mesh triangle references a missing vertex";
                    return false;
                }
                if (baseVertex + idx > std::numeric_limits<uint32_t>::max()) {
                    error = "MD5 mesh is too large for 32-bit indices";
                    return false;
                }
                meshData.indices.push_back(static_cast<uint32_t>(baseVertex + idx));
            }
        }

        const size_t indexCount = meshData.indices.size() - indexStart;
        if (indexCount > 0) {
            meshData.submeshes.emplace_back(
                indexStart,
                indexCount,
                static_cast<int>(meshData.submeshes.size()),
                md5Mesh.shader
            );
        }
    }

    for (size_t i = 0; i + 2 < meshData.indices.size(); i += 3) {
        addTriangleNormal(meshData, meshData.indices[i], meshData.indices[i + 1], meshData.indices[i + 2]);
    }
    normalizeNormals(meshData);
    meshData.computeBounds();

    if (meshData.vertices.empty() || meshData.indices.empty()) {
        error = "MD5 mesh produced no renderable geometry";
        return false;
    }

    return true;
}

} // namespace

Assets::AssetType QuakeMdlLoader::getAssetType() const
{
    return Assets::AssetType::Mesh;
}

std::vector<std::string> QuakeMdlLoader::getSupportedExtensions() const
{
    return { ".mdl", ".md5mesh" };
}

Assets::LoadResult QuakeMdlLoader::loadFromFile(const std::string& path,
                                                const Assets::LoadContext& /*context*/)
{
    Assets::LoadResult result;

    const std::string ext = lowerExtension(path);
    if (ext == ".md5mesh") {
        garden::assets::MD5Model model;
        if (!model.load(path)) {
            result.error_message = "QuakeImporter: failed to parse MD5 mesh (" + path + ")";
            return result;
        }

        auto meshData = std::make_shared<Assets::MeshAssetData>();
        meshData->source_path = path;

        std::string error;
        if (!buildMeshData(model, *meshData, error)) {
            result.error_message = "QuakeImporter: " + error + " (" + path + ")";
            return result;
        }

        result.success = true;
        result.data = meshData;
        return result;
    }

    result.error_message = "QuakeImporter: MDL parsing is not implemented yet (" + path + ")";
    return result;
}

bool QuakeMdlLoader::uploadToGPU(Assets::AssetData& data, IRenderAPI* render_api)
{
    if (!render_api) {
        LOG_ENGINE_ERROR("QuakeImporter: no render API for mesh upload");
        return false;
    }

    auto* meshPtr = std::get_if<std::shared_ptr<Assets::MeshAssetData>>(&data);
    if (!meshPtr || !*meshPtr) {
        LOG_ENGINE_ERROR("QuakeImporter: invalid mesh data for GPU upload");
        return false;
    }

    auto& meshData = **meshPtr;
    if (meshData.uploaded.load(std::memory_order_acquire)) {
        return true;
    }

    if (meshData.vertices.empty()) {
        LOG_ENGINE_ERROR("QuakeImporter: mesh has no vertices to upload");
        return false;
    }

    meshData.gpu_mesh = render_api->createMesh();
    if (!meshData.gpu_mesh) {
        LOG_ENGINE_ERROR("QuakeImporter: failed to create GPU mesh");
        return false;
    }

    if (meshData.use_indices && !meshData.indices.empty()) {
        meshData.gpu_mesh->uploadIndexedMeshData(
            meshData.vertices.data(),
            meshData.vertices.size(),
            meshData.indices.data(),
            meshData.indices.size()
        );
    } else {
        meshData.gpu_mesh->uploadMeshData(meshData.vertices.data(), meshData.vertices.size());
    }

    meshData.uploaded.store(true, std::memory_order_release);
    meshData.freeVertices();
    return true;
}

} // namespace QuakeImporter
