#include "AssetScanner.hpp"
#include "AssetMetadataSerializer.hpp"
#include "LODGenerator.hpp"
#include "LODMeshSerializer.hpp"
#include "Utils/FileHash.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/ObjLoader.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <chrono>

namespace fs = std::filesystem;

namespace Assets {

void AssetScanner::scanDirectory(const std::string& root_dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_scanned_assets.clear();

    if (!fs::exists(root_dir) || !fs::is_directory(root_dir))
    {
        printf("AssetScanner: Directory not found: %s\n", root_dir.c_str());
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(root_dir))
    {
        if (!entry.is_regular_file())
            continue;

        std::string path = entry.path().string();
        // Normalize path separators
        std::replace(path.begin(), path.end(), '\\', '/');

        if (!isMeshFile(path))
            continue;

        ScannedAsset asset;
        asset.source_path = path;
        asset.status = checkAsset(path);
        m_scanned_assets.push_back(std::move(asset));
    }

    printf("AssetScanner: Found %zu mesh assets\n", m_scanned_assets.size());
}

AssetScanStatus AssetScanner::checkAsset(const std::string& asset_path)
{
    std::string meta_path = AssetMetadataSerializer::getMetaPath(asset_path);

    if (!fs::exists(meta_path))
        return AssetScanStatus::NeedsScan;

    AssetMetadata existing;
    if (!AssetMetadataSerializer::load(existing, meta_path))
        return AssetScanStatus::NeedsScan;

    // Check if source file changed
    uint64_t current_hash = Utils::hashFile(asset_path);
    if (current_hash != existing.source_hash)
        return AssetScanStatus::NeedsUpdate;

    return AssetScanStatus::UpToDate;
}

// Internal: load mesh vertices from file
static bool loadMeshVertices(const std::string& asset_path, vertex*& out_vertices,
                             size_t& out_vertex_count, size_t& out_submesh_count,
                             std::vector<SubmeshInfo>& out_submeshes)
{
    std::string ext = fs::path(asset_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    out_vertices = nullptr;
    out_vertex_count = 0;
    out_submesh_count = 0;
    out_submeshes.clear();

    if (ext == ".gltf" || ext == ".glb")
    {
        GltfLoaderConfig config;
        config.verbose_logging = false;
        config.generate_normals_if_missing = true;
        config.flip_uvs = true;
        config.triangulate = true;

        GltfLoadResult result = GltfLoader::loadGltf(asset_path, config);
        if (result.success)
        {
            out_vertices = result.vertices;
            out_vertex_count = result.vertex_count;
            out_submesh_count = result.primitive_vertex_counts.size();

            // Build submesh info for per-submesh LOD generation
            size_t current_vertex = 0;
            for (size_t i = 0; i < result.primitive_vertex_counts.size(); ++i)
            {
                SubmeshInfo sub;
                sub.start_vertex = current_vertex;
                sub.vertex_count = result.primitive_vertex_counts[i];
                out_submeshes.push_back(sub);
                current_vertex += result.primitive_vertex_counts[i];
            }

            result.vertices = nullptr;
            return true;
        }
        printf("AssetScanner: Failed to load %s: %s\n", asset_path.c_str(), result.error_message.c_str());
    }
    else if (ext == ".obj")
    {
        ObjLoaderConfig config;
        config.verbose_logging = false;
        config.triangulate = true;

        ObjLoadResult result = ObjLoader::loadObj(asset_path, config);
        if (result.success)
        {
            out_vertices = result.vertices;
            out_vertex_count = result.vertex_count;
            out_submesh_count = 1;
            result.vertices = nullptr;
            return true;
        }
        printf("AssetScanner: Failed to load %s: %s\n", asset_path.c_str(), result.error_message.c_str());
    }

    return false;
}

// Internal: build metadata and save LOD files from generation results
static bool buildAndSaveMetadata(const std::string& asset_path,
                                  const LODGenerationResult& lod_result,
                                  const AssetMetadata::LODConfig& lod_config,
                                  const std::vector<float>& screen_thresholds,
                                  size_t submesh_count,
                                  const std::string& timestamp)
{
    AssetMetadata metadata;
    metadata.version = 1;
    metadata.source_path = fs::path(asset_path).filename().string();
    metadata.source_hash = Utils::hashFile(asset_path);
    metadata.source_file_size = Utils::getFileSize(asset_path);
    metadata.submesh_count = submesh_count;
    metadata.lod_enabled = true;
    metadata.lod_config = lod_config;
    metadata.generated_at = timestamp;

    if (!lod_result.lod_meshes.empty())
    {
        const auto& lod0 = lod_result.lod_meshes[0];
        metadata.vertex_count = lod0.vertices.size();
        metadata.index_count = lod0.indices.size();
        metadata.triangle_count = lod0.indices.size() / 3;

        if (!lod0.vertices.empty())
        {
            metadata.aabb_min = glm::vec3(lod0.vertices[0].vx, lod0.vertices[0].vy, lod0.vertices[0].vz);
            metadata.aabb_max = metadata.aabb_min;
            for (const auto& v : lod0.vertices)
            {
                metadata.aabb_min.x = std::min(metadata.aabb_min.x, v.vx);
                metadata.aabb_min.y = std::min(metadata.aabb_min.y, v.vy);
                metadata.aabb_min.z = std::min(metadata.aabb_min.z, v.vz);
                metadata.aabb_max.x = std::max(metadata.aabb_max.x, v.vx);
                metadata.aabb_max.y = std::max(metadata.aabb_max.y, v.vy);
                metadata.aabb_max.z = std::max(metadata.aabb_max.z, v.vz);
            }
        }
    }

    std::string dir = fs::path(asset_path).parent_path().string();
    std::string base = fs::path(asset_path).stem().string();

    for (size_t i = 0; i < lod_result.lod_meshes.size(); ++i)
    {
        const auto& lod = lod_result.lod_meshes[i];

        LODLevelInfo info;
        info.level = static_cast<int>(i);
        info.target_ratio = (i < lod_config.target_ratios.size()) ? lod_config.target_ratios[i] : lod.achieved_ratio;
        info.target_error = lod.achieved_error;
        info.vertex_count = lod.vertices.size();
        info.index_count = lod.indices.size();
        info.triangle_count = lod.indices.size() / 3;

        // Use provided screen thresholds if available, otherwise defaults
        if (i < screen_thresholds.size())
            info.screen_threshold = screen_thresholds[i];
        else
        {
            float default_thresholds[] = { 0.0f, 0.3f, 0.15f, 0.05f, 0.02f };
            info.screen_threshold = (i < 5) ? default_thresholds[i] : 0.01f;
        }

        if (i == 0)
        {
            info.file_path = "";
        }
        else
        {
            char lod_filename[256];
            snprintf(lod_filename, sizeof(lod_filename), "%s_lod%zu.lodbin", base.c_str(), i);
            info.file_path = lod_filename;

            std::string lod_path = dir + "/" + lod_filename;
            if (!LODMeshSerializer::save(lod, lod_path))
                printf("AssetScanner: Failed to save LOD%zu for %s\n", i, asset_path.c_str());
        }

        metadata.lod_levels.push_back(info);
    }

    std::string meta_path = AssetMetadataSerializer::getMetaPath(asset_path);
    if (!AssetMetadataSerializer::save(metadata, meta_path))
    {
        printf("AssetScanner: Failed to save metadata for %s\n", asset_path.c_str());
        return false;
    }

    printf("AssetScanner: Successfully processed %s (%zu LOD levels)\n",
           asset_path.c_str(), metadata.lod_levels.size());
    return true;
}

bool AssetScanner::processAsset(const std::string& asset_path)
{
    printf("AssetScanner: Processing %s\n", asset_path.c_str());

    vertex* vertices = nullptr;
    size_t vertex_count = 0;
    size_t submesh_count = 0;
    std::vector<SubmeshInfo> submeshes;

    if (!loadMeshVertices(asset_path, vertices, vertex_count, submesh_count, submeshes))
    {
        if (vertices) delete[] vertices;
        return false;
    }

    LODGenerationInput lod_input;
    lod_input.vertices = vertices;
    lod_input.vertex_count = vertex_count;
    lod_input.indices = nullptr;
    lod_input.index_count = 0;
    lod_input.config = m_default_lod_config;
    lod_input.submeshes = std::move(submeshes);

    LODGenerationResult lod_result = LODGenerator::generate(lod_input);
    delete[] vertices;

    if (!lod_result.success)
    {
        printf("AssetScanner: LOD generation failed for %s: %s\n",
               asset_path.c_str(), lod_result.error_message.c_str());
        return false;
    }

    return buildAndSaveMetadata(asset_path, lod_result, m_default_lod_config, {}, submesh_count, generateTimestamp());
}

bool AssetScanner::processAssetWithConfig(const std::string& asset_path,
                                           const AssetMetadata::LODConfig& config,
                                           const std::vector<float>& screen_thresholds)
{
    printf("AssetScanner: Processing %s with custom config (%d LOD levels)\n",
           asset_path.c_str(), config.max_lod_levels);

    vertex* vertices = nullptr;
    size_t vertex_count = 0;
    size_t submesh_count = 0;
    std::vector<SubmeshInfo> submeshes;

    if (!loadMeshVertices(asset_path, vertices, vertex_count, submesh_count, submeshes))
    {
        if (vertices) delete[] vertices;
        return false;
    }

    LODGenerationInput lod_input;
    lod_input.vertices = vertices;
    lod_input.vertex_count = vertex_count;
    lod_input.indices = nullptr;
    lod_input.index_count = 0;
    lod_input.config = config;
    lod_input.submeshes = std::move(submeshes);

    LODGenerationResult lod_result = LODGenerator::generate(lod_input);
    delete[] vertices;

    if (!lod_result.success)
    {
        printf("AssetScanner: LOD generation failed for %s: %s\n",
               asset_path.c_str(), lod_result.error_message.c_str());
        return false;
    }

    return buildAndSaveMetadata(asset_path, lod_result, config, screen_thresholds, submesh_count, generateTimestamp());
}

const std::vector<ScannedAsset>& AssetScanner::getScannedAssets() const
{
    return m_scanned_assets;
}

void AssetScanner::processAllPending()
{
    // Process synchronously for now; can be made async via JobSystem later
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& asset : m_scanned_assets)
    {
        if (asset.status == AssetScanStatus::NeedsScan ||
            asset.status == AssetScanStatus::NeedsUpdate)
        {
            asset.status = AssetScanStatus::Processing;
            if (processAsset(asset.source_path))
            {
                asset.status = AssetScanStatus::UpToDate;
                AssetMetadataSerializer::load(asset.metadata, AssetMetadataSerializer::getMetaPath(asset.source_path));
            }
            else
            {
                asset.status = AssetScanStatus::Error;
                asset.error_message = "Failed to process asset";
            }
        }
    }
}

bool AssetScanner::regenerateAsset(const std::string& asset_path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    bool success = processAsset(asset_path);

    // Update tracked status
    for (auto& asset : m_scanned_assets)
    {
        if (asset.source_path == asset_path)
        {
            if (success)
            {
                asset.status = AssetScanStatus::UpToDate;
                AssetMetadataSerializer::load(asset.metadata, AssetMetadataSerializer::getMetaPath(asset_path));
            }
            else
            {
                asset.status = AssetScanStatus::Error;
            }
            break;
        }
    }

    return success;
}

bool AssetScanner::isMeshFile(const std::string& path) const
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

std::string AssetScanner::getBaseName(const std::string& path) const
{
    fs::path p(path);
    return p.stem().string();
}

std::string AssetScanner::getDirectory(const std::string& path) const
{
    fs::path p(path);
    return p.parent_path().string();
}

std::string AssetScanner::generateTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(buf);
}

} // namespace Assets
