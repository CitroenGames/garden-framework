#include "CookedCollisionSerializer.hpp"
#include "PhysicsSystem.hpp"
#include "Utils/Log.hpp"

#include <Jolt/Core/Core.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <glm/geometric.hpp>

namespace fs = std::filesystem;

namespace Assets {

using JPH::uint64;

namespace {
    constexpr uint32_t GARDENPHYS_MAGIC = 0x59485047; // "GPHY" little endian

    struct GardenPhysHeader {
        uint32_t magic = GARDENPHYS_MAGIC;
        uint32_t version = CookedCollisionSerializer::FORMAT_VERSION;
        uint64_t jolt_version = JPH_VERSION_ID;
        uint64_t source_hash = 0;
        uint64_t source_file_size = 0;
        uint64_t vertex_count = 0;
        uint64_t triangle_count = 0;
        float aabb_min[3] = {0.0f, 0.0f, 0.0f};
        float aabb_max[3] = {0.0f, 0.0f, 0.0f};
    };

    bool isFiniteVertexPosition(const vertex& v)
    {
        return std::isfinite(v.vx) && std::isfinite(v.vy) && std::isfinite(v.vz);
    }

    bool isUsableTriangle(const vertex& v0, const vertex& v1, const vertex& v2)
    {
        if (!isFiniteVertexPosition(v0) || !isFiniteVertexPosition(v1) || !isFiniteVertexPosition(v2))
            return false;

        const glm::vec3 p0(v0.vx, v0.vy, v0.vz);
        const glm::vec3 p1(v1.vx, v1.vy, v1.vz);
        const glm::vec3 p2(v2.vx, v2.vy, v2.vz);
        const glm::vec3 area_vec = glm::cross(p1 - p0, p2 - p0);
        return glm::dot(area_vec, area_vec) > 0.0f;
    }

    std::string makeError(const std::string& prefix, const std::string& path)
    {
        return prefix + ": " + path;
    }

    bool buildShape(const LODMeshData& mesh_data,
                    JPH::ShapeRefC& out_shape,
                    GardenPhysHeader& header,
                    std::string* out_error)
    {
        if (mesh_data.vertices.empty())
        {
            if (out_error) *out_error = "collision mesh has no vertices";
            return false;
        }

        JPH::VertexList vertices;
        vertices.reserve(mesh_data.vertices.size());
        for (const vertex& v : mesh_data.vertices)
            vertices.push_back(JPH::Float3(v.vx, v.vy, v.vz));

        JPH::IndexedTriangleList triangles;
        if (!mesh_data.indices.empty())
        {
            triangles.reserve(mesh_data.indices.size() / 3);
            for (size_t i = 0; i + 2 < mesh_data.indices.size(); i += 3)
            {
                const uint32_t i0 = mesh_data.indices[i];
                const uint32_t i1 = mesh_data.indices[i + 1];
                const uint32_t i2 = mesh_data.indices[i + 2];
                if (i0 >= mesh_data.vertices.size() || i1 >= mesh_data.vertices.size() || i2 >= mesh_data.vertices.size())
                    continue;
                if (!isUsableTriangle(mesh_data.vertices[i0], mesh_data.vertices[i1], mesh_data.vertices[i2]))
                    continue;
                triangles.push_back(JPH::IndexedTriangle(i0, i1, i2, 0));
            }
        }
        else
        {
            triangles.reserve(mesh_data.vertices.size() / 3);
            for (size_t i = 0; i + 2 < mesh_data.vertices.size(); i += 3)
            {
                if (!isUsableTriangle(mesh_data.vertices[i], mesh_data.vertices[i + 1], mesh_data.vertices[i + 2]))
                    continue;
                triangles.push_back(JPH::IndexedTriangle(
                    static_cast<uint32_t>(i),
                    static_cast<uint32_t>(i + 1),
                    static_cast<uint32_t>(i + 2),
                    0));
            }
        }

        if (triangles.empty())
        {
            if (out_error) *out_error = "collision mesh has no usable triangles";
            return false;
        }

        JPH::MeshShapeSettings mesh_settings(std::move(vertices), std::move(triangles));
        mesh_settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
        JPH::ShapeSettings::ShapeResult result = mesh_settings.Create();
        if (!result.IsValid())
        {
            if (out_error) *out_error = result.GetError().c_str();
            return false;
        }

        header.vertex_count = static_cast<uint64_t>(mesh_data.vertices.size());
        header.triangle_count = static_cast<uint64_t>(result.Get()->GetStats().mNumTriangles);

        glm::vec3 bmin(std::numeric_limits<float>::max());
        glm::vec3 bmax(std::numeric_limits<float>::lowest());
        for (const vertex& v : mesh_data.vertices)
        {
            bmin = glm::min(bmin, glm::vec3(v.vx, v.vy, v.vz));
            bmax = glm::max(bmax, glm::vec3(v.vx, v.vy, v.vz));
        }
        header.aabb_min[0] = bmin.x;
        header.aabb_min[1] = bmin.y;
        header.aabb_min[2] = bmin.z;
        header.aabb_max[0] = bmax.x;
        header.aabb_max[1] = bmax.y;
        header.aabb_max[2] = bmax.z;

        out_shape = result.Get();
        return true;
    }
}

std::string CookedCollisionSerializer::defaultFileNameForAsset(const std::string& asset_path)
{
    fs::path path(asset_path);
    return path.stem().string() + ".gardenphys";
}

std::string CookedCollisionSerializer::defaultPathForAsset(const std::string& asset_path)
{
    fs::path path(asset_path);
    return (path.parent_path() / defaultFileNameForAsset(asset_path)).string();
}

bool CookedCollisionSerializer::cookMeshToFile(const LODMeshData& mesh_data,
                                               const std::string& output_path,
                                               uint64_t source_hash,
                                               uint64_t source_file_size,
                                               AssetMetadata::CollisionInfo* out_info,
                                               std::string* out_error)
{
    PhysicsSystem::ensureJoltRegistered();

    GardenPhysHeader header;
    header.source_hash = source_hash;
    header.source_file_size = source_file_size;

    JPH::ShapeRefC shape;
    if (!buildShape(mesh_data, shape, header, out_error))
        return false;

    fs::path output_fs_path(output_path);
    if (!output_fs_path.parent_path().empty())
        fs::create_directories(output_fs_path.parent_path());

    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open())
    {
        if (out_error) *out_error = makeError("failed to open cooked collision for writing", output_path);
        return false;
    }

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    JPH::StreamOutWrapper stream(file);
    shape->SaveBinaryState(stream);
    if (file.fail() || stream.IsFailed())
    {
        if (out_error) *out_error = makeError("failed to write cooked collision", output_path);
        return false;
    }

    if (out_info)
    {
        out_info->enabled = true;
        out_info->file_path = output_fs_path.filename().string();
        out_info->backend = "Jolt";
        out_info->cook_version = FORMAT_VERSION;
        out_info->source_hash = header.source_hash;
        out_info->source_file_size = header.source_file_size;
        out_info->jolt_version = header.jolt_version;
        out_info->vertex_count = static_cast<size_t>(header.vertex_count);
        out_info->triangle_count = static_cast<size_t>(header.triangle_count);
    }

    return true;
}

bool CookedCollisionSerializer::loadShape(const std::string& path,
                                          uint64_t expected_source_hash,
                                          uint64_t expected_source_file_size,
                                          JPH::ShapeRefC& out_shape,
                                          std::string* out_error)
{
    PhysicsSystem::ensureJoltRegistered();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        if (out_error) *out_error = makeError("failed to open cooked collision", path);
        return false;
    }

    GardenPhysHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.fail())
    {
        if (out_error) *out_error = makeError("failed to read cooked collision header", path);
        return false;
    }

    if (header.magic != GARDENPHYS_MAGIC || header.version != FORMAT_VERSION)
    {
        if (out_error) *out_error = makeError("unsupported cooked collision format", path);
        return false;
    }

    if (header.jolt_version != JPH_VERSION_ID)
    {
        if (out_error) *out_error = makeError("cooked collision Jolt version mismatch", path);
        return false;
    }

    if (expected_source_hash != 0 && header.source_hash != expected_source_hash)
    {
        if (out_error) *out_error = makeError("cooked collision source hash mismatch", path);
        return false;
    }

    if (expected_source_file_size != 0 && header.source_file_size != expected_source_file_size)
    {
        if (out_error) *out_error = makeError("cooked collision source size mismatch", path);
        return false;
    }

    JPH::StreamInWrapper stream(file);
    JPH::Shape::ShapeResult result = JPH::Shape::sRestoreFromBinaryState(stream);
    if (!result.IsValid())
    {
        if (out_error) *out_error = result.GetError().c_str();
        return false;
    }

    out_shape = result.Get();
    return out_shape != nullptr;
}

} // namespace Assets
