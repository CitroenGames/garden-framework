#include "PackageMeshDependencyExtractor.hpp"

#include "Assets/CompiledMeshFormat.hpp"
#include "Assets/CompiledMeshSerializer.hpp"
#include "Assets/CookedCollisionSerializer.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
bool isCmeshPath(const std::string& path)
{
    constexpr std::string_view suffix = ".cmesh";
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool appendTextureChild(std::vector<PackageDependencyRequest>& children,
                        const fs::path& mesh_path, const std::string& texture_path,
                        const std::string& owner, std::string& error)
{
    if (texture_path.empty())
        return true; // Embedded image payloads intentionally have no file edge.

    std::string logical_path;
    if (!normalizePackageLogicalPath((mesh_path.parent_path() / texture_path).generic_string(),
                                     logical_path))
    {
        error = "PKG-MESH-TEXTURE-PATH: " + texture_path + " required by " + owner;
        return false;
    }
    children.push_back({std::move(logical_path), PackageDependencyKind::Texture, owner,
                        std::string(PackageMeshDependencyExtractor::SourceId)});
    return true;
}
}

bool PackageMeshDependencyExtractor::extract(
    const std::string& mesh_logical_path, const std::string& staged_cmesh_path,
    std::vector<PackageDependencyRequest>& children, uint32_t& format_version,
    uint64_t& cook_fingerprint, std::string& error)
{
    children.clear();
    format_version = 0;
    cook_fingerprint = 0;
    error.clear();

    if (!isCmeshPath(mesh_logical_path))
    {
        error = "PKG-MESH-CONTRACT: " + mesh_logical_path +
                " must use the current .cmesh cooked contract";
        return false;
    }

    Assets::CompiledMeshData mesh;
    if (!Assets::CompiledMeshSerializer::load(mesh, staged_cmesh_path) ||
        mesh.header.magic != Assets::CMESH_MAGIC ||
        mesh.header.version != Assets::CMESH_VERSION)
    {
        error = "PKG-MESH-INVALID: " + mesh_logical_path;
        return false;
    }
    if (mesh.header.cook_fingerprint == 0)
    {
        error = "PKG-MESH-FINGERPRINT: " + mesh_logical_path;
        return false;
    }
    if (mesh.header.material_ref_count != mesh.material_refs.size() ||
        mesh.header.lod_count != mesh.lod_levels.size() || mesh.lod_levels.empty() ||
        mesh.lod_levels.front().vertices.empty())
    {
        error = "PKG-MESH-CONTRACT: " + mesh_logical_path;
        return false;
    }

    const fs::path mesh_path(mesh_logical_path);
    for (const Assets::CompiledMeshData::MaterialRef& material : mesh.material_refs)
    {
        // CMESH v4 owns material values directly.  Material names identify
        // records inside the artifact and are not external material paths.
        for (const Assets::CompiledMeshData::MaterialRef::TextureRef& texture : material.textures)
        {
            if (!appendTextureChild(children, mesh_path, texture.path, mesh_logical_path, error))
                return false;
        }
    }

    std::string collision_path;
    if (!normalizePackageLogicalPath(
            Assets::CookedCollisionSerializer::defaultPathForAsset(mesh_logical_path), collision_path))
    {
        error = "PKG-MESH-COLLISION-PATH: " + mesh_logical_path;
        return false;
    }
    children.push_back({std::move(collision_path), PackageDependencyKind::Collision,
                        mesh_logical_path, std::string(SourceId)});

    std::sort(children.begin(), children.end(), [](const PackageDependencyRequest& lhs,
                                                   const PackageDependencyRequest& rhs) {
        if (lhs.logical_path != rhs.logical_path)
            return lhs.logical_path < rhs.logical_path;
        return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
    });

    format_version = mesh.header.version;
    cook_fingerprint = mesh.header.cook_fingerprint;
    return true;
}
