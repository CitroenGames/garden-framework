#include "AssetMetadataSerializer.hpp"
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <ctime>
#include <cstdio>

using json = nlohmann::json;

namespace Assets {

bool AssetMetadataSerializer::save(const AssetMetadata& metadata, const std::string& meta_path)
{
    json j;

    j["version"] = metadata.version;

    // Source info
    j["source"]["path"] = metadata.source_path;
    j["source"]["hash"] = metadata.source_hash;
    j["source"]["file_size"] = metadata.source_file_size;

    // Mesh stats
    j["mesh"]["vertex_count"] = metadata.vertex_count;
    j["mesh"]["index_count"] = metadata.index_count;
    j["mesh"]["triangle_count"] = metadata.triangle_count;
    j["mesh"]["submesh_count"] = metadata.submesh_count;
    j["mesh"]["aabb_min"] = { metadata.aabb_min.x, metadata.aabb_min.y, metadata.aabb_min.z };
    j["mesh"]["aabb_max"] = { metadata.aabb_max.x, metadata.aabb_max.y, metadata.aabb_max.z };

    // LOD chain
    j["lod"]["enabled"] = metadata.lod_enabled;

    json lod_array = json::array();
    for (const auto& lod : metadata.lod_levels)
    {
        json lod_j;
        lod_j["level"] = lod.level;
        lod_j["ratio"] = lod.target_ratio;
        lod_j["error"] = lod.target_error;
        lod_j["threshold"] = lod.screen_threshold;
        lod_j["vertices"] = lod.vertex_count;
        lod_j["indices"] = lod.index_count;
        lod_j["triangles"] = lod.triangle_count;
        lod_j["file"] = lod.file_path;
        lod_array.push_back(lod_j);
    }
    j["lod"]["levels"] = lod_array;

    // LOD config
    j["lod"]["config"]["max_levels"] = metadata.lod_config.max_lod_levels;
    j["lod"]["config"]["target_ratios"] = metadata.lod_config.target_ratios;
    j["lod"]["config"]["error_threshold"] = metadata.lod_config.target_error_threshold;
    j["lod"]["config"]["lock_borders"] = metadata.lod_config.lock_borders;

    // Timestamp
    j["generated_at"] = metadata.generated_at;

    std::ofstream file(meta_path);
    if (!file.is_open())
    {
        printf("AssetMetadataSerializer: Failed to open %s for writing\n", meta_path.c_str());
        return false;
    }

    file << j.dump(2);
    return true;
}

bool AssetMetadataSerializer::load(AssetMetadata& metadata, const std::string& meta_path)
{
    std::ifstream file(meta_path);
    if (!file.is_open())
        return false;

    json j;
    try
    {
        file >> j;
    }
    catch (const json::parse_error& e)
    {
        printf("AssetMetadataSerializer: Parse error in %s: %s\n", meta_path.c_str(), e.what());
        return false;
    }

    metadata.version = j.value("version", 1);

    // Source info
    if (j.contains("source"))
    {
        const auto& src = j["source"];
        metadata.source_path = src.value("path", "");
        metadata.source_hash = src.value("hash", (uint64_t)0);
        metadata.source_file_size = src.value("file_size", (uint64_t)0);
    }

    // Mesh stats
    if (j.contains("mesh"))
    {
        const auto& mesh = j["mesh"];
        metadata.vertex_count = mesh.value("vertex_count", (size_t)0);
        metadata.index_count = mesh.value("index_count", (size_t)0);
        metadata.triangle_count = mesh.value("triangle_count", (size_t)0);
        metadata.submesh_count = mesh.value("submesh_count", (size_t)0);

        if (mesh.contains("aabb_min") && mesh["aabb_min"].is_array() && mesh["aabb_min"].size() == 3)
        {
            metadata.aabb_min = glm::vec3(
                mesh["aabb_min"][0].get<float>(),
                mesh["aabb_min"][1].get<float>(),
                mesh["aabb_min"][2].get<float>()
            );
        }
        if (mesh.contains("aabb_max") && mesh["aabb_max"].is_array() && mesh["aabb_max"].size() == 3)
        {
            metadata.aabb_max = glm::vec3(
                mesh["aabb_max"][0].get<float>(),
                mesh["aabb_max"][1].get<float>(),
                mesh["aabb_max"][2].get<float>()
            );
        }
    }

    // LOD chain
    if (j.contains("lod"))
    {
        const auto& lod = j["lod"];
        metadata.lod_enabled = lod.value("enabled", true);

        metadata.lod_levels.clear();
        if (lod.contains("levels") && lod["levels"].is_array())
        {
            for (const auto& lod_j : lod["levels"])
            {
                LODLevelInfo info;
                info.level = lod_j.value("level", 0);
                info.target_ratio = lod_j.value("ratio", 1.0f);
                info.target_error = lod_j.value("error", 0.0f);
                info.screen_threshold = lod_j.value("threshold", 0.0f);
                info.vertex_count = lod_j.value("vertices", (size_t)0);
                info.index_count = lod_j.value("indices", (size_t)0);
                info.triangle_count = lod_j.value("triangles", (size_t)0);
                info.file_path = lod_j.value("file", "");
                metadata.lod_levels.push_back(info);
            }
        }

        // LOD config
        if (lod.contains("config"))
        {
            const auto& cfg = lod["config"];
            metadata.lod_config.max_lod_levels = cfg.value("max_levels", 4);
            if (cfg.contains("target_ratios") && cfg["target_ratios"].is_array())
            {
                metadata.lod_config.target_ratios.clear();
                for (const auto& r : cfg["target_ratios"])
                    metadata.lod_config.target_ratios.push_back(r.get<float>());
            }
            metadata.lod_config.target_error_threshold = cfg.value("error_threshold", 0.01f);
            metadata.lod_config.lock_borders = cfg.value("lock_borders", false);
        }
    }

    metadata.generated_at = j.value("generated_at", "");

    return true;
}

bool AssetMetadataSerializer::exists(const std::string& asset_path)
{
    return std::filesystem::exists(getMetaPath(asset_path));
}

std::string AssetMetadataSerializer::getMetaPath(const std::string& asset_path)
{
    return asset_path + ".meta";
}

} // namespace Assets
