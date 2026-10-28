#include "AssetMetadataSerializer.hpp"
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using json = nlohmann::json;

namespace Assets {
namespace {
std::string temporarySiblingPath(const std::string& path)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return path + ".tmp." + std::to_string(nonce);
}

bool publishTemporaryFile(const std::string& temporary_path, const std::string& output_path)
{
#ifdef _WIN32
    if (MoveFileExA(temporary_path.c_str(), output_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return true;
#else
    std::error_code ec;
    std::filesystem::rename(temporary_path, output_path, ec);
    if (!ec)
        return true;
#endif
    std::error_code ignored;
    std::filesystem::remove(temporary_path, ignored);
    return false;
}

bool isNonNegativeInteger(const json& value)
{
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<int64_t>() >= 0);
}

bool isNumber(const json& value)
{
    return value.is_number();
}

bool optionalFieldHasType(const json& object, const char* name,
                          bool (*is_valid)(const json&))
{
    return !object.contains(name) || is_valid(object.at(name));
}

bool optionalVec3(const json& object, const char* name)
{
    if (!object.contains(name))
        return true;

    const json& value = object.at(name);
    return value.is_array() && value.size() == 3 &&
           isNumber(value[0]) && isNumber(value[1]) && isNumber(value[2]);
}

bool validateMetadataDocument(const json& document)
{
    if (!document.is_object())
        return false;

    // Metadata is a hard-cutover format. A missing, legacy, or future version is
    // not compatible with the current cooker and must be regenerated.
    if (!document.contains("version") ||
        !document.at("version").is_number_integer() ||
        document.at("version").get<int64_t>() != 3 ||
        !optionalFieldHasType(document, "generated_at", [](const json& value) {
            return value.is_string();
        }))
        return false;

    if (document.contains("source")) {
        const json& source = document.at("source");
        if (!source.is_object() ||
            !optionalFieldHasType(source, "path", [](const json& value) { return value.is_string(); }) ||
            !optionalFieldHasType(source, "hash", isNonNegativeInteger) ||
            !optionalFieldHasType(source, "file_size", isNonNegativeInteger) ||
            !optionalFieldHasType(source, "cook_fingerprint", isNonNegativeInteger))
            return false;
    }

    if (document.contains("mesh")) {
        const json& mesh = document.at("mesh");
        if (!mesh.is_object() ||
            !optionalFieldHasType(mesh, "vertex_count", isNonNegativeInteger) ||
            !optionalFieldHasType(mesh, "index_count", isNonNegativeInteger) ||
            !optionalFieldHasType(mesh, "triangle_count", isNonNegativeInteger) ||
            !optionalFieldHasType(mesh, "submesh_count", isNonNegativeInteger) ||
            !optionalVec3(mesh, "aabb_min") || !optionalVec3(mesh, "aabb_max"))
            return false;
    }

    if (document.contains("lod")) {
        const json& lod = document.at("lod");
        if (!lod.is_object() ||
            !optionalFieldHasType(lod, "enabled", [](const json& value) { return value.is_boolean(); }))
            return false;

        if (lod.contains("levels")) {
            const json& levels = lod.at("levels");
            if (!levels.is_array())
                return false;
            for (const json& level : levels) {
                if (!level.is_object() ||
                    !optionalFieldHasType(level, "level", [](const json& value) { return value.is_number_integer(); }) ||
                    !optionalFieldHasType(level, "ratio", isNumber) ||
                    !optionalFieldHasType(level, "error", isNumber) ||
                    !optionalFieldHasType(level, "threshold", isNumber) ||
                    !optionalFieldHasType(level, "vertices", isNonNegativeInteger) ||
                    !optionalFieldHasType(level, "indices", isNonNegativeInteger) ||
                    !optionalFieldHasType(level, "triangles", isNonNegativeInteger) ||
                    !optionalFieldHasType(level, "file", [](const json& value) { return value.is_string(); }))
                    return false;
            }
        }

        if (lod.contains("config")) {
            const json& config = lod.at("config");
            if (!config.is_object() ||
                !optionalFieldHasType(config, "max_levels", [](const json& value) { return value.is_number_integer(); }) ||
                !optionalFieldHasType(config, "error_threshold", isNumber) ||
                !optionalFieldHasType(config, "lock_borders", [](const json& value) { return value.is_boolean(); }) ||
                !optionalFieldHasType(config, "allow_attribute_collapse", [](const json& value) { return value.is_boolean(); }) ||
                !optionalFieldHasType(config, "prune_disconnected", [](const json& value) { return value.is_boolean(); }))
                return false;
            if (config.contains("target_ratios")) {
                const json& ratios = config.at("target_ratios");
                if (!ratios.is_array())
                    return false;
                for (const json& ratio : ratios) {
                    if (!isNumber(ratio))
                        return false;
                }
            }
        }
    }

    if (document.contains("collision")) {
        const json& collision = document.at("collision");
        if (!collision.is_object() ||
            !optionalFieldHasType(collision, "enabled", [](const json& value) { return value.is_boolean(); }) ||
            !optionalFieldHasType(collision, "file", [](const json& value) { return value.is_string(); }) ||
            !optionalFieldHasType(collision, "backend", [](const json& value) { return value.is_string(); }) ||
            !optionalFieldHasType(collision, "cook_version", isNonNegativeInteger) ||
            !optionalFieldHasType(collision, "source_hash", isNonNegativeInteger) ||
            !optionalFieldHasType(collision, "source_file_size", isNonNegativeInteger) ||
            !optionalFieldHasType(collision, "jolt_version", isNonNegativeInteger) ||
            !optionalFieldHasType(collision, "vertices", isNonNegativeInteger) ||
            !optionalFieldHasType(collision, "triangles", isNonNegativeInteger))
            return false;
    }

    return true;
}
} // namespace

bool AssetMetadataSerializer::save(const AssetMetadata& metadata, const std::string& meta_path)
{
    if (metadata.version != 3)
    {
        printf("AssetMetadataSerializer: Refusing to write non-current version %d to %s\n",
               metadata.version, meta_path.c_str());
        return false;
    }

    json j;

    j["version"] = 3;

    // Source info
    j["source"]["path"] = metadata.source_path;
    j["source"]["hash"] = metadata.source_hash;
    j["source"]["file_size"] = metadata.source_file_size;
    j["source"]["cook_fingerprint"] = metadata.cook_fingerprint;

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

    j["collision"]["enabled"] = metadata.collision.enabled;
    j["collision"]["file"] = metadata.collision.file_path;
    j["collision"]["backend"] = metadata.collision.backend;
    j["collision"]["cook_version"] = metadata.collision.cook_version;
    j["collision"]["source_hash"] = metadata.collision.source_hash;
    j["collision"]["source_file_size"] = metadata.collision.source_file_size;
    j["collision"]["jolt_version"] = metadata.collision.jolt_version;
    j["collision"]["vertices"] = metadata.collision.vertex_count;
    j["collision"]["triangles"] = metadata.collision.triangle_count;

    // LOD config
    j["lod"]["config"]["max_levels"] = metadata.lod_config.max_lod_levels;
    j["lod"]["config"]["target_ratios"] = metadata.lod_config.target_ratios;
    j["lod"]["config"]["error_threshold"] = metadata.lod_config.target_error_threshold;
    j["lod"]["config"]["lock_borders"] = metadata.lod_config.lock_borders;
    j["lod"]["config"]["allow_attribute_collapse"] = metadata.lod_config.allow_attribute_collapse;
    j["lod"]["config"]["prune_disconnected"] = metadata.lod_config.prune_disconnected;

    // Timestamp
    j["generated_at"] = metadata.generated_at;

    const std::string temporary_path = temporarySiblingPath(meta_path);
    std::ofstream file(temporary_path, std::ios::binary);
    if (!file.is_open())
    {
        printf("AssetMetadataSerializer: Failed to open %s for writing\n", temporary_path.c_str());
        return false;
    }

    file << j.dump(2);
    file.close();
    if (!file || !publishTemporaryFile(temporary_path, meta_path))
    {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        printf("AssetMetadataSerializer: Failed to atomically publish %s\n", meta_path.c_str());
        return false;
    }
    return true;
}

bool AssetMetadataSerializer::load(AssetMetadata& metadata, const std::string& meta_path)
{
    std::ifstream file(meta_path);
    if (!file.is_open())
        return false;

    // Asset data is untrusted input and this target disables C++ exceptions. Use the
    // non-throwing parser and validate every accessed field before deserializing it.
    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !validateMetadataDocument(j))
    {
        printf("AssetMetadataSerializer: Invalid metadata in %s\n", meta_path.c_str());
        return false;
    }

    metadata.version = 3;

    // Source info
    if (j.contains("source"))
    {
        const auto& src = j["source"];
        metadata.source_path = src.value("path", "");
        metadata.source_hash = src.value("hash", (uint64_t)0);
        metadata.source_file_size = src.value("file_size", (uint64_t)0);
        metadata.cook_fingerprint = src.value("cook_fingerprint", (uint64_t)0);
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
            metadata.lod_config.target_error_threshold = cfg.value("error_threshold", 0.05f);
            metadata.lod_config.lock_borders = cfg.value("lock_borders", false);
            metadata.lod_config.allow_attribute_collapse = cfg.value("allow_attribute_collapse", false);
            metadata.lod_config.prune_disconnected = cfg.value("prune_disconnected", false);
        }
    }

    if (j.contains("collision"))
    {
        const auto& collision = j["collision"];
        metadata.collision.enabled = collision.value("enabled", false);
        metadata.collision.file_path = collision.value("file", "");
        metadata.collision.backend = collision.value("backend", "Jolt");
        metadata.collision.cook_version = collision.value("cook_version", (uint32_t)1);
        metadata.collision.source_hash = collision.value("source_hash", (uint64_t)0);
        metadata.collision.source_file_size = collision.value("source_file_size", (uint64_t)0);
        metadata.collision.jolt_version = collision.value("jolt_version", (uint64_t)0);
        metadata.collision.vertex_count = collision.value("vertices", (size_t)0);
        metadata.collision.triangle_count = collision.value("triangles", (size_t)0);
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
