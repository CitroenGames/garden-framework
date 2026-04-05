#include "LevelManager.hpp"
#include "json.hpp"
#include "Components/Components.hpp"
#include "world.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/GltfMaterialLoader.hpp"
#include "Utils/Log.hpp"
#include "Assets/AssetMetadataSerializer.hpp"
#include "Assets/LODMeshSerializer.hpp"
#include <iostream>
#include <cstring>
#include <filesystem>

using json = nlohmann::json;

static constexpr uint32_t LEVEL_BINARY_MAGIC   = 0x47444E4C; // "GDNL"
static constexpr uint32_t LEVEL_BINARY_VERSION  = 3;

// Helper to get texture type name for logging (moved from main.cpp)
static std::string getTextureTypeName(TextureType type) {
    switch (type) {
        case TextureType::BASE_COLOR: return "Base Color";
        case TextureType::METALLIC_ROUGHNESS: return "Metallic-Roughness";
        case TextureType::NORMAL: return "Normal";
        case TextureType::OCCLUSION: return "Occlusion";
        case TextureType::EMISSIVE: return "Emissive";
        case TextureType::DIFFUSE: return "Diffuse";
        case TextureType::SPECULAR: return "Specular";
        default: return "Unknown";
    }
}

LevelManager::LevelManager()
{
}

LevelManager::~LevelManager()
{
    cleanup();
}

void LevelManager::cleanup()
{
    stored_entities.clear();
}

bool LevelManager::loadLevel(const std::string& path, LevelData& out_level_data)
{
    // Auto-detect format by extension
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json")
    {
        return loadLevelFromJSON(path, out_level_data);
    }
    else if (path.size() >= 11 && path.substr(path.size() - 11) == ".level.json")
    {
        return loadLevelFromJSON(path, out_level_data);
    }
    else if (path.size() >= 6 && path.substr(path.size() - 6) == ".level")
    {
        return loadLevelFromBinary(path, out_level_data);
    }
    else
    {
        printf("ERROR: Unknown level file format: %s\n", path.c_str());
        return false;
    }
}

bool LevelManager::loadLevelFromJSON(const std::string& json_path, LevelData& out_level_data)
{
    printf("Loading level from JSON: %s\n", json_path.c_str());

    std::ifstream file(json_path);
    if (!file.is_open())
    {
        printf("ERROR: Could not open JSON file: %s\n", json_path.c_str());
        return false;
    }

    json j;
    try
    {
        file >> j;
    }
    catch (const json::exception& e)
    {
        printf("ERROR: Failed to parse JSON: %s\n", e.what());
        return false;
    }

    // Parse metadata
    if (!parseMetadataFromJSON(&j, out_level_data.metadata))
    {
        printf("ERROR: Failed to parse metadata\n");
        return false;
    }

    // Parse entities
    if (j.contains("entities") && j["entities"].is_array())
    {
        for (const auto& entity_json : j["entities"])
        {
            LevelEntity entity;
            if (parseEntityFromJSON(&entity_json, entity))
            {
                out_level_data.entities.push_back(entity);
            }
        }
    }

    out_level_data.metadata.entity_count = (int)out_level_data.entities.size();
    printf("Loaded %d entities from level\n", out_level_data.metadata.entity_count);

    return true;
}

bool LevelManager::parseMetadataFromJSON(const void* json_ptr, LevelMetadata& metadata)
{
    const json& j = *static_cast<const json*>(json_ptr);

    if (!j.contains("metadata"))
    {
        printf("WARNING: No metadata section found, using defaults\n");
        return true;
    }

    const auto& meta = j["metadata"];

    if (meta.contains("level_name")) metadata.level_name = meta["level_name"].get<std::string>();
    if (meta.contains("author")) metadata.author = meta["author"].get<std::string>();
    if (meta.contains("version")) metadata.version = meta["version"].get<std::string>();

    // Parse world settings
    if (meta.contains("world"))
    {
        const auto& world_settings = meta["world"];
        if (world_settings.contains("gravity"))
        {
            const auto& g = world_settings["gravity"];
            metadata.gravity = glm::vec3(
                g.value("x", 0.0f),
                g.value("y", -1.0f),
                g.value("z", 0.0f)
            );
        }
        if (world_settings.contains("fixed_delta"))
        {
            metadata.fixed_delta = world_settings["fixed_delta"].get<float>();
        }
    }

    // Parse lighting
    if (meta.contains("lighting"))
    {
        const auto& lighting = meta["lighting"];
        if (lighting.contains("ambient"))
        {
            const auto& a = lighting["ambient"];
            metadata.ambient_light = glm::vec3(
                a.value("r", 0.2f),
                a.value("g", 0.2f),
                a.value("b", 0.2f)
            );
        }
        if (lighting.contains("diffuse"))
        {
            const auto& d = lighting["diffuse"];
            metadata.diffuse_light = glm::vec3(
                d.value("r", 0.8f),
                d.value("g", 0.8f),
                d.value("b", 0.8f)
            );
        }
        if (lighting.contains("direction"))
        {
            const auto& dir = lighting["direction"];
            float x = dir.value("x", 0.0f);
            float y = dir.value("y", -1.0f);
            float z = dir.value("z", 0.0f);
            metadata.light_direction = glm::vec3(x, y, z);
            // Normalize direction
            float length = std::sqrt(x*x + y*y + z*z);
            if (length > 0.0001f) {
                metadata.light_direction = glm::normalize(metadata.light_direction);
            }
        }
        // Backward compatibility: fallback to position if direction not specified
        else if (lighting.contains("position"))
        {
            const auto& pos = lighting["position"];
            float x = pos.value("x", 0.0f);
            float y = pos.value("y", -1.0f);
            float z = pos.value("z", 0.0f);
            // Convert position to direction (pointing toward origin)
            metadata.light_direction = glm::vec3(-x, -y, -z);
            // Normalize
            float length = std::sqrt(x*x + y*y + z*z);
            if (length > 0.0001f) {
                metadata.light_direction = glm::normalize(metadata.light_direction);
            }
        }
    }

    return true;
}

bool LevelManager::parseEntityFromJSON(const void* json_ptr, LevelEntity& entity)
{
    const json& j = *static_cast<const json*>(json_ptr);

    // Parse name and type
    if (j.contains("name")) entity.name = j["name"].get<std::string>();

    if (j.contains("type"))
    {
        std::string type_str = j["type"].get<std::string>();
        if (type_str == "Static") entity.type = EntityType::Static;
        else if (type_str == "Renderable") entity.type = EntityType::Renderable;
        else if (type_str == "Collidable") entity.type = EntityType::Collidable;
        else if (type_str == "Physical") entity.type = EntityType::Physical;
        else if (type_str == "Player") entity.type = EntityType::Player;
        else if (type_str == "Freecam") entity.type = EntityType::Freecam;
        else if (type_str == "PlayerRep") entity.type = EntityType::PlayerRep;
        else if (type_str == "PointLight") entity.type = EntityType::PointLight;
        else if (type_str == "SpotLight") entity.type = EntityType::SpotLight;
    }

    // Parse transform
    if (j.contains("transform"))
    {
        const auto& transform = j["transform"];
        if (transform.contains("position"))
        {
            const auto& pos = transform["position"];
            entity.position = glm::vec3(
                pos.value("x", 0.0f),
                pos.value("y", 0.0f),
                pos.value("z", 0.0f)
            );
        }
        if (transform.contains("rotation"))
        {
            const auto& rot = transform["rotation"];
            entity.rotation = glm::vec3(
                rot.value("x", 0.0f),
                rot.value("y", 0.0f),
                rot.value("z", 0.0f)
            );
        }
        if (transform.contains("scale"))
        {
            const auto& scl = transform["scale"];
            entity.scale = glm::vec3(
                scl.value("x", 1.0f),
                scl.value("y", 1.0f),
                scl.value("z", 1.0f)
            );
        }
    }

    // Parse mesh
    if (j.contains("mesh"))
    {
        const auto& mesh_data = j["mesh"];
        if (mesh_data.contains("path")) entity.mesh_path = mesh_data["path"].get<std::string>();

        if (mesh_data.contains("textures") && mesh_data["textures"].is_array())
        {
            for (const auto& tex : mesh_data["textures"])
            {
                entity.texture_paths.push_back(tex.get<std::string>());
            }
        }

        entity.culling = mesh_data.value("culling", true);
        entity.transparent = mesh_data.value("transparent", false);
        entity.visible = mesh_data.value("visible", true);
        entity.casts_shadow = mesh_data.value("casts_shadow", true);
        entity.force_lod = mesh_data.value("force_lod", -1);
        entity.use_mesh_collision = mesh_data.value("use_mesh_collision", false);
    }

    // Parse rigidbody
    if (j.contains("rigidbody"))
    {
        entity.has_rigidbody = true;
        const auto& rb = j["rigidbody"];
        entity.mass = rb.value("mass", 1.0f);
        entity.apply_gravity = rb.value("apply_gravity", true);
    }

    // Parse collider
    if (j.contains("collider"))
    {
        entity.has_collider = true;
        const auto& col = j["collider"];
        if (col.contains("mesh_path"))
        {
            entity.collider_mesh_path = col["mesh_path"].get<std::string>();
        }
    }

    // Parse player properties
    if (j.contains("player_properties"))
    {
        const auto& pp = j["player_properties"];
        entity.speed = pp.value("speed", 1.5f);
        entity.jump_force = pp.value("jump_force", 3.0f);
        entity.mouse_sensitivity = pp.value("mouse_sensitivity", 1.0f);
        entity.apply_gravity = pp.value("apply_gravity", false);
        entity.has_rigidbody = true; // Player always has rigidbody
    }

    // Parse freecam properties
    if (j.contains("freecam_properties"))
    {
        const auto& fp = j["freecam_properties"];
        entity.movement_speed = fp.value("movement_speed", 5.0f);
        entity.fast_movement_speed = fp.value("fast_movement_speed", 15.0f);
        entity.mouse_sensitivity = fp.value("mouse_sensitivity", 1.0f);
    }

    // Parse player rep properties
    if (j.contains("player_rep_properties"))
    {
        const auto& pr = j["player_rep_properties"];
        if (pr.contains("tracked_player"))
        {
            entity.tracked_player_name = pr["tracked_player"].get<std::string>();
        }
        if (pr.contains("position_offset"))
        {
            const auto& offset = pr["position_offset"];
            entity.position_offset = glm::vec3(
                offset.value("x", 0.0f),
                offset.value("y", 0.0f),
                offset.value("z", 0.0f)
            );
        }
    }

    // Parse point light properties
    if (j.contains("point_light"))
    {
        const auto& pl = j["point_light"];
        if (pl.contains("color"))
        {
            const auto& c = pl["color"];
            entity.light_color = glm::vec3(c.value("r", 1.0f), c.value("g", 1.0f), c.value("b", 1.0f));
        }
        entity.light_intensity = pl.value("intensity", 1.0f);
        entity.light_range = pl.value("range", 10.0f);
        entity.light_constant_attenuation = pl.value("constant_attenuation", 1.0f);
        entity.light_linear_attenuation = pl.value("linear_attenuation", 0.09f);
        entity.light_quadratic_attenuation = pl.value("quadratic_attenuation", 0.032f);
    }

    // Parse spot light properties
    if (j.contains("spot_light"))
    {
        const auto& sl = j["spot_light"];
        if (sl.contains("color"))
        {
            const auto& c = sl["color"];
            entity.light_color = glm::vec3(c.value("r", 1.0f), c.value("g", 1.0f), c.value("b", 1.0f));
        }
        entity.light_intensity = sl.value("intensity", 1.0f);
        entity.light_range = sl.value("range", 15.0f);
        entity.light_inner_cone_angle = sl.value("inner_cone_angle", 12.5f);
        entity.light_outer_cone_angle = sl.value("outer_cone_angle", 17.5f);
        entity.light_constant_attenuation = sl.value("constant_attenuation", 1.0f);
        entity.light_linear_attenuation = sl.value("linear_attenuation", 0.09f);
        entity.light_quadratic_attenuation = sl.value("quadratic_attenuation", 0.032f);
    }

    return true;
}

static std::string entityTypeToString(EntityType type)
{
    switch (type)
    {
    case EntityType::Static:     return "Static";
    case EntityType::Renderable: return "Renderable";
    case EntityType::Collidable: return "Collidable";
    case EntityType::Physical:   return "Physical";
    case EntityType::Player:     return "Player";
    case EntityType::Freecam:    return "Freecam";
    case EntityType::PlayerRep:  return "PlayerRep";
    case EntityType::PointLight: return "PointLight";
    case EntityType::SpotLight:  return "SpotLight";
    default:                     return "Static";
    }
}

bool LevelManager::saveLevelToJSON(const std::string& json_path, const LevelData& level_data)
{
    json j;

    // --- Metadata ---
    const LevelMetadata& meta = level_data.metadata;
    j["metadata"]["level_name"] = meta.level_name;
    j["metadata"]["author"]     = meta.author;
    j["metadata"]["version"]    = meta.version;
    j["metadata"]["world"]["gravity"] = {
        {"x", meta.gravity.x}, {"y", meta.gravity.y}, {"z", meta.gravity.z}
    };
    j["metadata"]["world"]["fixed_delta"] = meta.fixed_delta;
    j["metadata"]["lighting"]["ambient"] = {
        {"r", meta.ambient_light.x}, {"g", meta.ambient_light.y}, {"b", meta.ambient_light.z}
    };
    j["metadata"]["lighting"]["diffuse"] = {
        {"r", meta.diffuse_light.x}, {"g", meta.diffuse_light.y}, {"b", meta.diffuse_light.z}
    };
    j["metadata"]["lighting"]["direction"] = {
        {"x", meta.light_direction.x}, {"y", meta.light_direction.y}, {"z", meta.light_direction.z}
    };

    // --- Entities ---
    json entities_array = json::array();
    for (const auto& le : level_data.entities)
    {
        json e;
        e["name"] = le.name;
        e["type"] = entityTypeToString(le.type);
        e["transform"]["position"] = {{"x", le.position.x}, {"y", le.position.y}, {"z", le.position.z}};
        e["transform"]["rotation"] = {{"x", le.rotation.x}, {"y", le.rotation.y}, {"z", le.rotation.z}};
        e["transform"]["scale"]    = {{"x", le.scale.x},    {"y", le.scale.y},    {"z", le.scale.z}};

        if (!le.mesh_path.empty())
        {
            e["mesh"]["path"]              = le.mesh_path;
            e["mesh"]["textures"]          = le.texture_paths;
            e["mesh"]["culling"]           = le.culling;
            e["mesh"]["transparent"]       = le.transparent;
            e["mesh"]["visible"]           = le.visible;
            e["mesh"]["casts_shadow"]      = le.casts_shadow;
            e["mesh"]["force_lod"]         = le.force_lod;
            e["mesh"]["use_mesh_collision"]= le.use_mesh_collision;
        }

        if (le.has_rigidbody)
        {
            e["rigidbody"]["mass"]         = le.mass;
            e["rigidbody"]["apply_gravity"]= le.apply_gravity;
        }

        if (le.has_collider && !le.collider_mesh_path.empty())
        {
            e["collider"]["mesh_path"] = le.collider_mesh_path;
        }

        if (le.type == EntityType::Player)
        {
            e["player_properties"]["speed"]              = le.speed;
            e["player_properties"]["jump_force"]         = le.jump_force;
            e["player_properties"]["mouse_sensitivity"]  = le.mouse_sensitivity;
            e["player_properties"]["apply_gravity"]      = le.apply_gravity;
        }

        if (le.type == EntityType::Freecam)
        {
            e["freecam_properties"]["movement_speed"]      = le.movement_speed;
            e["freecam_properties"]["fast_movement_speed"] = le.fast_movement_speed;
            e["freecam_properties"]["mouse_sensitivity"]   = le.mouse_sensitivity;
        }

        if (le.type == EntityType::PlayerRep)
        {
            e["player_rep_properties"]["tracked_player"] = le.tracked_player_name;
            e["player_rep_properties"]["position_offset"] = {
                {"x", le.position_offset.x},
                {"y", le.position_offset.y},
                {"z", le.position_offset.z}
            };
        }

        if (le.type == EntityType::PointLight)
        {
            e["point_light"]["color"] = {{"r", le.light_color.x}, {"g", le.light_color.y}, {"b", le.light_color.z}};
            e["point_light"]["intensity"] = le.light_intensity;
            e["point_light"]["range"] = le.light_range;
            e["point_light"]["constant_attenuation"] = le.light_constant_attenuation;
            e["point_light"]["linear_attenuation"] = le.light_linear_attenuation;
            e["point_light"]["quadratic_attenuation"] = le.light_quadratic_attenuation;
        }

        if (le.type == EntityType::SpotLight)
        {
            e["spot_light"]["color"] = {{"r", le.light_color.x}, {"g", le.light_color.y}, {"b", le.light_color.z}};
            e["spot_light"]["intensity"] = le.light_intensity;
            e["spot_light"]["range"] = le.light_range;
            e["spot_light"]["inner_cone_angle"] = le.light_inner_cone_angle;
            e["spot_light"]["outer_cone_angle"] = le.light_outer_cone_angle;
            e["spot_light"]["constant_attenuation"] = le.light_constant_attenuation;
            e["spot_light"]["linear_attenuation"] = le.light_linear_attenuation;
            e["spot_light"]["quadratic_attenuation"] = le.light_quadratic_attenuation;
        }

        entities_array.push_back(e);
    }
    j["entities"] = entities_array;

    std::ofstream out_file(json_path);
    if (!out_file.is_open())
    {
        printf("ERROR: Could not open file for writing: %s\n", json_path.c_str());
        return false;
    }
    out_file << j.dump(2);
    printf("Saved level to: %s (%d entities)\n", json_path.c_str(), (int)level_data.entities.size());
    return true;
}

bool LevelManager::loadLevelFromBinary(const std::string& binary_path, LevelData& out_level_data)
{
    printf("Loading level from binary: %s\n", binary_path.c_str());

    std::ifstream file(binary_path, std::ios::binary);
    if (!file.is_open())
    {
        printf("ERROR: Could not open binary level file: %s\n", binary_path.c_str());
        return false;
    }

    if (!readBinaryHeader(file, out_level_data.metadata))
    {
        printf("ERROR: Failed to read binary level header: %s\n", binary_path.c_str());
        return false;
    }

    int count = out_level_data.metadata.entity_count;
    out_level_data.entities.reserve(count);

    for (int i = 0; i < count; i++)
    {
        LevelEntity entity;
        if (!readBinaryEntity(file, entity))
        {
            printf("ERROR: Failed to read entity %d from binary level: %s\n", i, binary_path.c_str());
            return false;
        }
        out_level_data.entities.push_back(std::move(entity));
    }

    printf("Loaded %d entities from binary level\n", count);
    return true;
}

bool LevelManager::saveLevelToBinary(const std::string& binary_path, const LevelData& level_data)
{
    std::ofstream file(binary_path, std::ios::binary);
    if (!file.is_open())
    {
        printf("ERROR: Could not open file for binary writing: %s\n", binary_path.c_str());
        return false;
    }

    LevelMetadata meta_copy = level_data.metadata;
    meta_copy.entity_count = (int)level_data.entities.size();

    writeBinaryHeader(file, meta_copy);

    for (const auto& entity : level_data.entities)
    {
        writeBinaryEntity(file, entity);
    }

    printf("Saved binary level to: %s (%d entities)\n", binary_path.c_str(), (int)level_data.entities.size());
    return file.good();
}

bool LevelManager::compileLevel(const std::string& json_path, const std::string& binary_path)
{
    LevelData level_data;
    if (!loadLevelFromJSON(json_path, level_data))
    {
        return false;
    }

    return saveLevelToBinary(binary_path, level_data);
}

void LevelManager::writeString(std::ofstream& file, const std::string& str)
{
    uint32_t length = (uint32_t)str.length();
    file.write(reinterpret_cast<const char*>(&length), sizeof(length));
    if (length > 0)
    {
        file.write(str.c_str(), length);
    }
}

bool LevelManager::readString(std::ifstream& file, std::string& str)
{
    uint32_t length;
    file.read(reinterpret_cast<char*>(&length), sizeof(length));
    if (file.fail()) return false;

    if (length > 0)
    {
        str.resize(length);
        file.read(&str[0], length);
        if (file.fail()) return false;
    }
    else
    {
        str.clear();
    }

    return true;
}

void LevelManager::writeBinaryHeader(std::ofstream& file, const LevelMetadata& metadata)
{
    file.write(reinterpret_cast<const char*>(&LEVEL_BINARY_MAGIC), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&LEVEL_BINARY_VERSION), sizeof(uint32_t));

    writeString(file, metadata.level_name);
    writeString(file, metadata.author);
    writeString(file, metadata.version);

    uint32_t entity_count = (uint32_t)metadata.entity_count;
    file.write(reinterpret_cast<const char*>(&entity_count), sizeof(uint32_t));

    file.write(reinterpret_cast<const char*>(&metadata.gravity), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&metadata.fixed_delta), sizeof(float));
    file.write(reinterpret_cast<const char*>(&metadata.ambient_light), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&metadata.diffuse_light), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&metadata.light_direction), sizeof(glm::vec3));
}

void LevelManager::writeBinaryEntity(std::ofstream& file, const LevelEntity& entity)
{
    writeString(file, entity.name);

    uint8_t type = static_cast<uint8_t>(entity.type);
    file.write(reinterpret_cast<const char*>(&type), sizeof(uint8_t));

    file.write(reinterpret_cast<const char*>(&entity.position), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.rotation), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.scale), sizeof(glm::vec3));

    writeString(file, entity.mesh_path);

    uint32_t texture_count = (uint32_t)entity.texture_paths.size();
    file.write(reinterpret_cast<const char*>(&texture_count), sizeof(uint32_t));
    for (const auto& tex : entity.texture_paths)
    {
        writeString(file, tex);
    }

    uint8_t has_rigidbody    = entity.has_rigidbody ? 1 : 0;
    uint8_t apply_gravity    = entity.apply_gravity ? 1 : 0;
    uint8_t has_collider     = entity.has_collider ? 1 : 0;
    uint8_t use_mesh_col     = entity.use_mesh_collision ? 1 : 0;
    uint8_t culling          = entity.culling ? 1 : 0;
    uint8_t transparent      = entity.transparent ? 1 : 0;
    uint8_t visible          = entity.visible ? 1 : 0;
    uint8_t casts_shadow     = entity.casts_shadow ? 1 : 0;

    file.write(reinterpret_cast<const char*>(&has_rigidbody), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&entity.mass), sizeof(float));
    file.write(reinterpret_cast<const char*>(&apply_gravity), sizeof(uint8_t));

    file.write(reinterpret_cast<const char*>(&has_collider), sizeof(uint8_t));
    writeString(file, entity.collider_mesh_path);
    file.write(reinterpret_cast<const char*>(&use_mesh_col), sizeof(uint8_t));

    file.write(reinterpret_cast<const char*>(&culling), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&transparent), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&visible), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&casts_shadow), sizeof(uint8_t));

    int32_t force_lod = entity.force_lod;
    file.write(reinterpret_cast<const char*>(&force_lod), sizeof(int32_t));

    file.write(reinterpret_cast<const char*>(&entity.speed), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.jump_force), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.mouse_sensitivity), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.movement_speed), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.fast_movement_speed), sizeof(float));

    writeString(file, entity.tracked_player_name);
    file.write(reinterpret_cast<const char*>(&entity.position_offset), sizeof(glm::vec3));

    // Light data
    file.write(reinterpret_cast<const char*>(&entity.light_color), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.light_intensity), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_range), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_constant_attenuation), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_linear_attenuation), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_quadratic_attenuation), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_inner_cone_angle), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.light_outer_cone_angle), sizeof(float));
}

bool LevelManager::readBinaryHeader(std::ifstream& file, LevelMetadata& metadata)
{
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
    if (file.fail() || magic != LEVEL_BINARY_MAGIC)
    {
        printf("ERROR: Invalid binary level magic (expected 0x%08X, got 0x%08X)\n", LEVEL_BINARY_MAGIC, magic);
        return false;
    }

    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    if (file.fail() || version != LEVEL_BINARY_VERSION)
    {
        printf("ERROR: Unsupported binary level version (expected %u, got %u)\n", LEVEL_BINARY_VERSION, version);
        return false;
    }

    if (!readString(file, metadata.level_name)) return false;
    if (!readString(file, metadata.author)) return false;
    if (!readString(file, metadata.version)) return false;

    uint32_t entity_count;
    file.read(reinterpret_cast<char*>(&entity_count), sizeof(uint32_t));
    if (file.fail()) return false;
    metadata.entity_count = (int)entity_count;

    file.read(reinterpret_cast<char*>(&metadata.gravity), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&metadata.fixed_delta), sizeof(float));
    file.read(reinterpret_cast<char*>(&metadata.ambient_light), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&metadata.diffuse_light), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&metadata.light_direction), sizeof(glm::vec3));

    return !file.fail();
}

bool LevelManager::readBinaryEntity(std::ifstream& file, LevelEntity& entity)
{
    if (!readString(file, entity.name)) return false;

    uint8_t type;
    file.read(reinterpret_cast<char*>(&type), sizeof(uint8_t));
    if (file.fail()) return false;
    entity.type = static_cast<EntityType>(type);

    file.read(reinterpret_cast<char*>(&entity.position), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&entity.rotation), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&entity.scale), sizeof(glm::vec3));
    if (file.fail()) return false;

    if (!readString(file, entity.mesh_path)) return false;

    uint32_t texture_count;
    file.read(reinterpret_cast<char*>(&texture_count), sizeof(uint32_t));
    if (file.fail() || texture_count > 1024) return false;

    entity.texture_paths.resize(texture_count);
    for (uint32_t i = 0; i < texture_count; i++)
    {
        if (!readString(file, entity.texture_paths[i])) return false;
    }

    uint8_t has_rigidbody, apply_gravity, has_collider, use_mesh_col;
    uint8_t culling, transparent, visible, casts_shadow;

    file.read(reinterpret_cast<char*>(&has_rigidbody), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&entity.mass), sizeof(float));
    file.read(reinterpret_cast<char*>(&apply_gravity), sizeof(uint8_t));

    file.read(reinterpret_cast<char*>(&has_collider), sizeof(uint8_t));
    if (!readString(file, entity.collider_mesh_path)) return false;
    file.read(reinterpret_cast<char*>(&use_mesh_col), sizeof(uint8_t));

    file.read(reinterpret_cast<char*>(&culling), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&transparent), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&visible), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&casts_shadow), sizeof(uint8_t));
    if (file.fail()) return false;

    entity.has_rigidbody    = has_rigidbody != 0;
    entity.apply_gravity    = apply_gravity != 0;
    entity.has_collider     = has_collider != 0;
    entity.use_mesh_collision = use_mesh_col != 0;
    entity.culling          = culling != 0;
    entity.transparent      = transparent != 0;
    entity.visible          = visible != 0;
    entity.casts_shadow     = casts_shadow != 0;

    int32_t force_lod;
    file.read(reinterpret_cast<char*>(&force_lod), sizeof(int32_t));
    if (file.fail()) return false;
    entity.force_lod = force_lod;

    file.read(reinterpret_cast<char*>(&entity.speed), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.jump_force), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.mouse_sensitivity), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.movement_speed), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.fast_movement_speed), sizeof(float));
    if (file.fail()) return false;

    if (!readString(file, entity.tracked_player_name)) return false;
    file.read(reinterpret_cast<char*>(&entity.position_offset), sizeof(glm::vec3));

    // Light data (v3+)
    file.read(reinterpret_cast<char*>(&entity.light_color), sizeof(glm::vec3));
    file.read(reinterpret_cast<char*>(&entity.light_intensity), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_range), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_constant_attenuation), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_linear_attenuation), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_quadratic_attenuation), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_inner_cone_angle), sizeof(float));
    file.read(reinterpret_cast<char*>(&entity.light_outer_cone_angle), sizeof(float));

    return !file.fail();
}

std::shared_ptr<mesh> LevelManager::loadMesh(const LevelEntity& entity, IRenderAPI* render_api)
{
    if (entity.mesh_path.empty())
    {
        return nullptr;
    }

    std::shared_ptr<mesh> m_ptr;

    // Check if glTF file (which might have materials)
    size_t path_len = entity.mesh_path.size();
    bool is_gltf = (path_len >= 5 && entity.mesh_path.substr(path_len - 5) == ".gltf") ||
                   (path_len >= 4 && entity.mesh_path.substr(path_len - 4) == ".glb");

    if (is_gltf)
    {
        // Configure geometry loading
        GltfLoaderConfig gltf_config;
        gltf_config.verbose_logging = true;
        gltf_config.flip_uvs = true;
        gltf_config.generate_normals_if_missing = true;
        gltf_config.scale = 1.0f;

        // Configure material loading
        MaterialLoaderConfig material_config;
        material_config.verbose_logging = true;
        material_config.load_all_textures = false;
        material_config.priority_texture_types = {
            TextureType::BASE_COLOR,
            TextureType::DIFFUSE,
            TextureType::NORMAL
        };
        material_config.generate_mipmaps = true;
        material_config.flip_textures_vertically = true;
        material_config.cache_textures = true;
        // Derive texture base path from the mesh file's directory
        {
            size_t last_sep = entity.mesh_path.find_last_of("/\\");
            material_config.texture_base_path = (last_sep != std::string::npos)
                ? entity.mesh_path.substr(0, last_sep + 1)
                : "";
        }

        // Load geometry and materials
        GltfLoadResult map_result = GltfLoader::loadGltfWithMaterials(entity.mesh_path, render_api, gltf_config, material_config);

        if (!map_result.success) {
            LOG_ENGINE_FATAL("Failed to load glTF file: %s\n", map_result.error_message.c_str());
            return nullptr;
        }

        LOG_ENGINE_TRACE("Loaded glTF: {}", entity.mesh_path.c_str());
        
        // Create mesh from glTF data
        m_ptr = std::make_shared<mesh>(map_result.vertices, map_result.vertex_count);

        // Apply textures
        bool texture_applied = false;

        if (map_result.materials_loaded && !map_result.material_data.materials.empty()) {
            std::vector<MaterialRange> material_ranges;
            size_t current_vertex = 0;

            for (size_t i = 0; i < map_result.material_indices.size(); ++i) {
                int mat_idx = map_result.material_indices[i];
                size_t vertex_count = map_result.primitive_vertex_counts[i];

                if (mat_idx >= 0 && mat_idx < map_result.material_data.materials.size()) {
                    const auto& material = map_result.material_data.materials[mat_idx];
                    TextureHandle tex = material.getPrimaryTextureHandle();

                    MaterialRange range(current_vertex, vertex_count, tex, material.properties.name);
                    material_ranges.push_back(range);

                    if (tex != INVALID_TEXTURE) {
                        texture_applied = true;
                    }
                }
                else {
                    MaterialRange range(current_vertex, vertex_count, INVALID_TEXTURE, "unknown");
                    material_ranges.push_back(range);
                }

                current_vertex += vertex_count;
            }

            if (!material_ranges.empty()) {
                m_ptr->setMaterialRanges(material_ranges);
            }
        }

        // Fallback texture
        if (!texture_applied) { 
             // Logic for fallback? For now just log
             LOG_ENGINE_WARN("No valid textures found in materials for %s\n", entity.mesh_path.c_str());
             
             // Try legacy fallback or load manually
             if (!entity.texture_paths.empty() && render_api) {
                 TextureHandle tex = render_api->loadTexture(entity.texture_paths[0], true, true);
                 m_ptr->set_texture(tex);
             }
        }
        
        // Transfer ownership cleanup
        map_result.vertices = nullptr; 
        map_result.vertex_count = 0;
    }
    else
    {
        // Regular mesh loading (OBJ)
        m_ptr = std::make_shared<mesh>(entity.mesh_path);
        
        // Load textures
        if (!entity.texture_paths.empty() && render_api)
        {
            for (const auto& tex_path : entity.texture_paths)
            {
                TextureHandle tex = render_api->loadTexture(tex_path, true, true);
                m_ptr->set_texture(tex);
                break; // Use first texture
            }
        }
    }

    if (m_ptr)
    {
        m_ptr->culling = entity.culling;
        m_ptr->transparent = entity.transparent;
        m_ptr->visible = entity.visible;
        m_ptr->casts_shadow = entity.casts_shadow;
        m_ptr->force_lod = entity.force_lod;

        // Load LOD data from .meta file if available
        if (render_api && !entity.mesh_path.empty())
        {
            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(entity.mesh_path);
            Assets::AssetMetadata metadata;
            if (Assets::AssetMetadataSerializer::load(metadata, meta_path) && metadata.lod_enabled)
            {
                std::string mesh_dir = std::filesystem::path(entity.mesh_path).parent_path().string();
                if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
                    mesh_dir += "/";

                for (size_t i = 1; i < metadata.lod_levels.size(); ++i)
                {
                    const auto& lod_info = metadata.lod_levels[i];
                    if (lod_info.file_path.empty()) continue;

                    std::string lod_path = mesh_dir + lod_info.file_path;
                    Assets::LODMeshData lod_data;
                    if (Assets::LODMeshSerializer::load(lod_data, lod_path))
                    {
                        mesh::LODLevel level;
                        level.screen_threshold = lod_info.screen_threshold;
                        level.vertex_count = lod_data.vertices.size();
                        level.index_count = lod_data.indices.size();
                        level.gpu_mesh = render_api->createMesh();
                        if (level.gpu_mesh)
                        {
                            level.gpu_mesh->uploadIndexedMeshData(
                                lod_data.vertices.data(), lod_data.vertices.size(),
                                lod_data.indices.data(), lod_data.indices.size()
                            );
                        }

                        // Map LOD submesh ranges to original mesh's material textures
                        if (!lod_data.submesh_ranges.empty() && m_ptr->uses_material_ranges)
                        {
                            for (const auto& sr : lod_data.submesh_ranges)
                            {
                                TextureHandle tex = INVALID_TEXTURE;
                                std::string mat_name = "";
                                if (sr.submesh_id < m_ptr->material_ranges.size())
                                {
                                    tex = m_ptr->material_ranges[sr.submesh_id].texture;
                                    mat_name = m_ptr->material_ranges[sr.submesh_id].material_name;
                                }
                                level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
                            }
                        }

                        m_ptr->lod_levels.push_back(std::move(level));
                    }
                }

                if (!m_ptr->lod_levels.empty())
                {
                    m_ptr->computeBounds();
                    LOG_ENGINE_TRACE("Loaded {} LOD levels for {}", m_ptr->lod_levels.size(), entity.mesh_path);
                }
            }
        }
    }

    return m_ptr;
}

bool LevelManager::instantiateLevel(
    const LevelData& level_data,
    world& game_world,
    IRenderAPI* render_api,
    entt::entity* out_player_entity,
    entt::entity* out_freecam_entity,
    entt::entity* out_player_rep_entity)
{
    printf("Instantiating level: %s\n", level_data.metadata.level_name.c_str());

    // Apply world settings
    game_world.setGravity(level_data.metadata.gravity);
    game_world.setFixedDelta(level_data.metadata.fixed_delta);

    // Initialize output pointers
    if (out_player_entity) *out_player_entity = entt::null;
    if (out_freecam_entity) *out_freecam_entity = entt::null;
    if (out_player_rep_entity) *out_player_rep_entity = entt::null;

    // Map to store entities by name for reference resolution
    std::map<std::string, entt::entity> entity_map;
    
    // Store created entities corresponding to level_data.entities indices
    std::vector<entt::entity> created_entities;
    created_entities.reserve(level_data.entities.size());

    // Create all entities
    for (const auto& entity_data : level_data.entities)
    {
        // Create entity in registry
        auto e = game_world.registry.create();
        created_entities.push_back(e);
        
        if (!entity_data.name.empty()) {
            entity_map[entity_data.name] = e;
        }

        // Add Transform
        game_world.registry.emplace<TransformComponent>(e, entity_data.position.x, entity_data.position.y, entity_data.position.z);
        auto& transform = game_world.registry.get<TransformComponent>(e);
        transform.rotation = entity_data.rotation;
        transform.scale = entity_data.scale;

        // Add Tag
        game_world.registry.emplace<TagComponent>(e, entity_data.name);

        // Load and add Mesh
        if (entity_data.type == EntityType::Renderable ||
            entity_data.type == EntityType::Physical ||
            entity_data.type == EntityType::PlayerRep)
        {
            if (!entity_data.mesh_path.empty()) { 
                auto mesh_ptr = loadMesh(entity_data, render_api);
                if (mesh_ptr) {
                    game_world.registry.emplace<MeshComponent>(e, mesh_ptr);
                }
            }
        }

        // Renderable with explicit collider (e.g. map with separate collision mesh)
        if (entity_data.type == EntityType::Renderable && entity_data.has_collider)
        {
            game_world.registry.emplace<ColliderComponent>(e);
            auto& col = game_world.registry.get<ColliderComponent>(e);
            if (!entity_data.collider_mesh_path.empty()) {
                LevelEntity col_ent = entity_data;
                col_ent.mesh_path = entity_data.collider_mesh_path;
                col_ent.texture_paths.clear();
                col.m_mesh = loadMesh(col_ent, render_api);
            } else if (game_world.registry.all_of<MeshComponent>(e)) {
                col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
            }
        }

        // Add Physics components
        if (entity_data.type == EntityType::Physical ||
            entity_data.type == EntityType::Player)
        {
            // Rigidbody
            if (entity_data.has_rigidbody) {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = entity_data.mass;
                rb.apply_gravity = entity_data.apply_gravity;
            }

            // Collider
            if (entity_data.has_collider) {
                game_world.registry.emplace<ColliderComponent>(e);
                auto& col = game_world.registry.get<ColliderComponent>(e);
                
                // If it has a separate collider mesh
                if (!entity_data.collider_mesh_path.empty()) {
                    LevelEntity col_ent = entity_data;
                    col_ent.mesh_path = entity_data.collider_mesh_path;
                    col_ent.texture_paths.clear(); // No texture for collider
                    col.m_mesh = loadMesh(col_ent, render_api);
                } else if (game_world.registry.all_of<MeshComponent>(e)) {
                    // Share visual mesh
                    col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
                }
            }
        }
        
        // Collidable only (static collider)
        if (entity_data.type == EntityType::Collidable) {
             if (entity_data.has_collider) {
                game_world.registry.emplace<ColliderComponent>(e);
                auto& col = game_world.registry.get<ColliderComponent>(e);
                
                if (!entity_data.collider_mesh_path.empty()) {
                    LevelEntity col_ent = entity_data;
                    col_ent.mesh_path = entity_data.collider_mesh_path;
                    col.m_mesh = loadMesh(col_ent, render_api);
                } else if (game_world.registry.all_of<MeshComponent>(e)) {
                    col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
                }
            }
        }

        // Auto-generate collision from visual mesh if use_mesh_collision is set
        if (entity_data.use_mesh_collision && !game_world.registry.all_of<ColliderComponent>(e)
            && game_world.registry.all_of<MeshComponent>(e))
        {
            game_world.registry.emplace<ColliderComponent>(e);
            auto& col = game_world.registry.get<ColliderComponent>(e);
            col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
            LOG_ENGINE_INFO("Entity '{}': using visual mesh as collision (use_mesh_collision=true)", entity_data.name);
        }

        // Register collider meshes with Jolt physics
        if (game_world.registry.all_of<ColliderComponent>(e))
        {
            auto& col = game_world.registry.get<ColliderComponent>(e);
            auto& t = game_world.registry.get<TransformComponent>(e);
            LOG_ENGINE_INFO("Jolt: entity '{}' has ColliderComponent, mesh_valid={}, vertices={}, len={}",
                entity_data.name, col.is_mesh_valid(),
                (void*)( col.m_mesh ? col.m_mesh->vertices : nullptr),
                col.m_mesh ? col.m_mesh->vertices_len : 0);
            if (col.is_mesh_valid())
            {
                if (entity_data.type == EntityType::Physical)
                {
                    // Dynamic body with capsule shape for physical entities
                    JPH::CapsuleShapeSettings capsule(0.5f, 0.3f);
                    auto shape_result = capsule.Create();
                    if (shape_result.IsValid())
                    {
                        game_world.getPhysicsSystem().createDynamicBody(
                            t.position, t.rotation, shape_result.Get(), entity_data.mass, e);
                    }
                }
                else
                {
                    // Static mesh collider
                    game_world.getPhysicsSystem().createStaticMeshBody(
                        t.position, t.rotation, *col.get_mesh(), e);
                }
            }
        }

        // Player
        if (entity_data.type == EntityType::Player)
        {
            game_world.registry.emplace<PlayerComponent>(e);
            auto& pc = game_world.registry.get<PlayerComponent>(e);
            pc.speed = entity_data.speed;
            pc.jump_force = entity_data.jump_force;
            pc.mouse_sensitivity = entity_data.mouse_sensitivity;

            // Ensure player has a RigidBodyComponent
            if (!game_world.registry.all_of<RigidBodyComponent>(e))
            {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = 80.0f;
                rb.apply_gravity = true;
            }

            // Create Jolt capsule body for player collision
            {
                auto& t = game_world.registry.get<TransformComponent>(e);
                auto& pc = game_world.registry.get<PlayerComponent>(e);
                JPH::CapsuleShapeSettings capsule(pc.capsule_half_height, pc.capsule_radius);
                auto shape_result = capsule.Create();
                if (shape_result.IsValid())
                {
                    game_world.getPhysicsSystem().createDynamicBody(
                        t.position, t.rotation, shape_result.Get(), 80.0f, e);
                }
            }
            
            if (out_player_entity) *out_player_entity = e;
            printf("NOTE: Player entity '%s' created\n", entity_data.name.c_str());
        }

        // Freecam
        if (entity_data.type == EntityType::Freecam)
        {
            game_world.registry.emplace<FreecamComponent>(e);
            auto& fc = game_world.registry.get<FreecamComponent>(e);
            fc.movement_speed = entity_data.movement_speed;
            fc.fast_movement_speed = entity_data.fast_movement_speed;
            fc.mouse_sensitivity = entity_data.mouse_sensitivity;
            
            if (out_freecam_entity) *out_freecam_entity = e;
            printf("NOTE: Freecam entity '%s' created\n", entity_data.name.c_str());
        }

        // Player Rep
        if (entity_data.type == EntityType::PlayerRep)
        {
             game_world.registry.emplace<PlayerRepresentationComponent>(e);
             auto& pr = game_world.registry.get<PlayerRepresentationComponent>(e);
             pr.position_offset = entity_data.position_offset;
             // tracked_player resolved in second pass

             if (out_player_rep_entity) *out_player_rep_entity = e;
        }

        if (entity_data.type == EntityType::PointLight)
        {
            auto& pl = game_world.registry.emplace<PointLightComponent>(e);
            pl.color = entity_data.light_color;
            pl.intensity = entity_data.light_intensity;
            pl.range = entity_data.light_range;
            pl.constant_attenuation = entity_data.light_constant_attenuation;
            pl.linear_attenuation = entity_data.light_linear_attenuation;
            pl.quadratic_attenuation = entity_data.light_quadratic_attenuation;
        }

        if (entity_data.type == EntityType::SpotLight)
        {
            auto& sl = game_world.registry.emplace<SpotLightComponent>(e);
            sl.color = entity_data.light_color;
            sl.intensity = entity_data.light_intensity;
            sl.range = entity_data.light_range;
            sl.inner_cone_angle = entity_data.light_inner_cone_angle;
            sl.outer_cone_angle = entity_data.light_outer_cone_angle;
            sl.constant_attenuation = entity_data.light_constant_attenuation;
            sl.linear_attenuation = entity_data.light_linear_attenuation;
            sl.quadratic_attenuation = entity_data.light_quadratic_attenuation;
        }
    }
    
    // Second pass: Resolve references (PlayerRepresentation)
    for (size_t i = 0; i < level_data.entities.size(); ++i) {
        const auto& entity_data = level_data.entities[i];
        if (entity_data.type == EntityType::PlayerRep) {
            entt::entity e = created_entities[i];
            auto& pr = game_world.registry.get<PlayerRepresentationComponent>(e);
            
            if (!entity_data.tracked_player_name.empty()) {
                auto it = entity_map.find(entity_data.tracked_player_name);
                if (it != entity_map.end()) {
                    pr.tracked_player = it->second;
                } else {
                    printf("WARNING: PlayerRepresentation '%s' cannot find tracked player '%s'\n", 
                           entity_data.name.c_str(), entity_data.tracked_player_name.c_str());
                }
            }
        }
    }

    printf("Level instantiation complete: %d entities\n", (int)level_data.entities.size());
    return true;
}