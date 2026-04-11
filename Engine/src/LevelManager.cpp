#include "LevelManager.hpp"
#include "json.hpp"
#include "Components/Components.hpp"
#include "world.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/GltfMaterialLoader.hpp"
#include "Utils/Log.hpp"
#include "Assets/AssetMetadataSerializer.hpp"
#include "Assets/AssetMetadata.hpp"
#include "Assets/LODMeshSerializer.hpp"
#include "Assets/CompiledMeshSerializer.hpp"
#include "Assets/CompiledTextureSerializer.hpp"
#include "Assets/AssetManager.hpp"
#include "Threading/JobSystem.hpp"
#include <iostream>
#include <cstring>
#include <filesystem>

using json = nlohmann::json;

static constexpr uint32_t LEVEL_BINARY_MAGIC   = 0x47444E4C; // "GDNL"
static constexpr uint32_t LEVEL_BINARY_VERSION  = 4;
static constexpr uint32_t LEVEL_BINARY_VERSION_V3 = 3; // Previous version for backward compat

// stringToColliderShapeType and colliderShapeTypeToString are inline in Components.hpp

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
        entity.body_motion_type = rb.value("motion_type", "Dynamic");
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

        // Shape type (defaults to "Mesh" for backward compat)
        entity.collider_shape_type = col.value("shape_type", "Mesh");

        if (col.contains("box_half_extents")) {
            const auto& b = col["box_half_extents"];
            entity.collider_box_half_extents = glm::vec3(b.value("x", 0.5f), b.value("y", 0.5f), b.value("z", 0.5f));
        }
        entity.collider_sphere_radius = col.value("sphere_radius", 0.5f);
        entity.collider_capsule_half_height = col.value("capsule_half_height", 0.5f);
        entity.collider_capsule_radius = col.value("capsule_radius", 0.3f);
        entity.collider_cylinder_half_height = col.value("cylinder_half_height", 0.5f);
        entity.collider_cylinder_radius = col.value("cylinder_radius", 0.5f);
        entity.collider_friction = col.value("friction", 0.2f);
        entity.collider_restitution = col.value("restitution", 0.0f);
    }

    // Parse constraint
    if (j.contains("constraint"))
    {
        entity.has_constraint = true;
        const auto& con = j["constraint"];
        entity.constraint_type = con.value("type", "Fixed");
        if (con.contains("target"))
            entity.constraint_target_name = con["target"].get<std::string>();
        if (con.contains("anchor_1")) {
            const auto& a = con["anchor_1"];
            entity.constraint_anchor_1 = glm::vec3(a.value("x", 0.0f), a.value("y", 0.0f), a.value("z", 0.0f));
        }
        if (con.contains("anchor_2")) {
            const auto& a = con["anchor_2"];
            entity.constraint_anchor_2 = glm::vec3(a.value("x", 0.0f), a.value("y", 0.0f), a.value("z", 0.0f));
        }
        if (con.contains("hinge_axis")) {
            const auto& a = con["hinge_axis"];
            entity.constraint_hinge_axis = glm::vec3(a.value("x", 0.0f), a.value("y", 1.0f), a.value("z", 0.0f));
        }
        entity.constraint_hinge_min = con.value("min_limit", -180.0f);
        entity.constraint_hinge_max = con.value("max_limit", 180.0f);
        entity.constraint_min_distance = con.value("min_distance", -1.0f);
        entity.constraint_max_distance = con.value("max_distance", -1.0f);
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
            e["rigidbody"]["motion_type"]  = le.body_motion_type;
        }

        if (le.has_collider)
        {
            if (!le.collider_mesh_path.empty())
                e["collider"]["mesh_path"] = le.collider_mesh_path;

            e["collider"]["shape_type"] = le.collider_shape_type;

            if (le.collider_shape_type == "Box") {
                e["collider"]["box_half_extents"] = {
                    {"x", le.collider_box_half_extents.x},
                    {"y", le.collider_box_half_extents.y},
                    {"z", le.collider_box_half_extents.z}
                };
            }
            if (le.collider_shape_type == "Sphere")
                e["collider"]["sphere_radius"] = le.collider_sphere_radius;
            if (le.collider_shape_type == "Capsule") {
                e["collider"]["capsule_half_height"] = le.collider_capsule_half_height;
                e["collider"]["capsule_radius"] = le.collider_capsule_radius;
            }
            if (le.collider_shape_type == "Cylinder") {
                e["collider"]["cylinder_half_height"] = le.collider_cylinder_half_height;
                e["collider"]["cylinder_radius"] = le.collider_cylinder_radius;
            }

            e["collider"]["friction"] = le.collider_friction;
            e["collider"]["restitution"] = le.collider_restitution;
        }

        if (le.has_constraint)
        {
            e["constraint"]["type"] = le.constraint_type;
            e["constraint"]["target"] = le.constraint_target_name;
            e["constraint"]["anchor_1"] = {{"x", le.constraint_anchor_1.x}, {"y", le.constraint_anchor_1.y}, {"z", le.constraint_anchor_1.z}};
            e["constraint"]["anchor_2"] = {{"x", le.constraint_anchor_2.x}, {"y", le.constraint_anchor_2.y}, {"z", le.constraint_anchor_2.z}};
            if (le.constraint_type == "Hinge") {
                e["constraint"]["hinge_axis"] = {{"x", le.constraint_hinge_axis.x}, {"y", le.constraint_hinge_axis.y}, {"z", le.constraint_hinge_axis.z}};
                e["constraint"]["min_limit"] = le.constraint_hinge_min;
                e["constraint"]["max_limit"] = le.constraint_hinge_max;
            }
            if (le.constraint_type == "Distance") {
                e["constraint"]["min_distance"] = le.constraint_min_distance;
                e["constraint"]["max_distance"] = le.constraint_max_distance;
            }
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

    // v4: Motion type, collider shape, and constraint data
    writeString(file, entity.body_motion_type);
    writeString(file, entity.collider_shape_type);
    file.write(reinterpret_cast<const char*>(&entity.collider_box_half_extents), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.collider_sphere_radius), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_capsule_half_height), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_capsule_radius), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_cylinder_half_height), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_cylinder_radius), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_friction), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.collider_restitution), sizeof(float));

    // Constraint data
    uint8_t has_constraint = entity.has_constraint ? 1 : 0;
    file.write(reinterpret_cast<const char*>(&has_constraint), sizeof(uint8_t));
    writeString(file, entity.constraint_type);
    writeString(file, entity.constraint_target_name);
    file.write(reinterpret_cast<const char*>(&entity.constraint_anchor_1), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.constraint_anchor_2), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.constraint_hinge_axis), sizeof(glm::vec3));
    file.write(reinterpret_cast<const char*>(&entity.constraint_hinge_min), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.constraint_hinge_max), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.constraint_min_distance), sizeof(float));
    file.write(reinterpret_cast<const char*>(&entity.constraint_max_distance), sizeof(float));
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
    if (file.fail() || (version != LEVEL_BINARY_VERSION && version != LEVEL_BINARY_VERSION_V3))
    {
        printf("ERROR: Unsupported binary level version (expected %u or %u, got %u)\n", LEVEL_BINARY_VERSION, LEVEL_BINARY_VERSION_V3, version);
        return false;
    }
    binary_read_version = version;

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

    // v4: Motion type and collider shape data
    if (binary_read_version >= LEVEL_BINARY_VERSION)
    {
        if (!readString(file, entity.body_motion_type)) return false;
        if (!readString(file, entity.collider_shape_type)) return false;
        file.read(reinterpret_cast<char*>(&entity.collider_box_half_extents), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&entity.collider_sphere_radius), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_capsule_half_height), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_capsule_radius), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_cylinder_half_height), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_cylinder_radius), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_friction), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.collider_restitution), sizeof(float));

        // Constraint data
        uint8_t has_constraint;
        file.read(reinterpret_cast<char*>(&has_constraint), sizeof(uint8_t));
        entity.has_constraint = has_constraint != 0;
        if (!readString(file, entity.constraint_type)) return false;
        if (!readString(file, entity.constraint_target_name)) return false;
        file.read(reinterpret_cast<char*>(&entity.constraint_anchor_1), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&entity.constraint_anchor_2), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&entity.constraint_hinge_axis), sizeof(glm::vec3));
        file.read(reinterpret_cast<char*>(&entity.constraint_hinge_min), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.constraint_hinge_max), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.constraint_min_distance), sizeof(float));
        file.read(reinterpret_cast<char*>(&entity.constraint_max_distance), sizeof(float));
    }

    return !file.fail();
}

std::shared_ptr<mesh> LevelManager::loadMesh(const LevelEntity& entity, IRenderAPI* render_api)
{
    if (entity.mesh_path.empty())
    {
        return nullptr;
    }

    // Resolve mesh path through AssetManager
    std::string resolved_path = Assets::AssetManager::get().resolveAssetPath(entity.mesh_path);

    // Try compiled mesh (.cmesh) first
    {
        std::filesystem::path p(resolved_path);
        std::string cmesh_path = (p.parent_path() / p.stem()).string() + ".cmesh";
        if (std::filesystem::exists(cmesh_path))
        {
            auto compiled = loadCompiledMesh(cmesh_path, render_api);
            if (compiled)
            {
                compiled->culling = entity.culling;
                compiled->transparent = entity.transparent;
                compiled->visible = entity.visible;
                compiled->casts_shadow = entity.casts_shadow;
                compiled->force_lod = entity.force_lod;
                return compiled;
            }
        }
    }

    std::shared_ptr<mesh> m_ptr;

    // Check if glTF file (which might have materials)
    size_t path_len = resolved_path.size();
    bool is_gltf = (path_len >= 5 && resolved_path.substr(path_len - 5) == ".gltf") ||
                   (path_len >= 4 && resolved_path.substr(path_len - 4) == ".glb");

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
            size_t last_sep = resolved_path.find_last_of("/\\");
            material_config.texture_base_path = (last_sep != std::string::npos)
                ? resolved_path.substr(0, last_sep + 1)
                : "";
        }

        // Load geometry and materials
        GltfLoadResult map_result = GltfLoader::loadGltfWithMaterials(resolved_path, render_api, gltf_config, material_config);

        if (!map_result.success) {
            LOG_ENGINE_FATAL("Failed to load glTF file: %s\n", map_result.error_message.c_str());
            return nullptr;
        }

        LOG_ENGINE_TRACE("Loaded glTF: {}", resolved_path.c_str());
        
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
             LOG_ENGINE_WARN("No valid textures found in materials for %s\n", resolved_path.c_str());

             // Try legacy fallback or load manually
             if (!entity.texture_paths.empty() && render_api) {
                 std::string tex_resolved = Assets::AssetManager::get().resolveAssetPath(entity.texture_paths[0]);
                 TextureHandle tex = render_api->loadTexture(tex_resolved, true, true);
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
        m_ptr = std::make_shared<mesh>(resolved_path);

        // Load textures
        if (!entity.texture_paths.empty() && render_api)
        {
            for (const auto& tex_path : entity.texture_paths)
            {
                std::string tex_resolved = Assets::AssetManager::get().resolveAssetPath(tex_path);
                TextureHandle tex = render_api->loadTexture(tex_resolved, true, true);
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
        if (render_api && !resolved_path.empty())
        {
            std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(resolved_path);
            Assets::AssetMetadata metadata;
            if (Assets::AssetMetadataSerializer::load(metadata, meta_path) && metadata.lod_enabled)
            {
                std::string mesh_dir = std::filesystem::path(resolved_path).parent_path().string();
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
                    LOG_ENGINE_TRACE("Loaded {} LOD levels for {}", m_ptr->lod_levels.size(), resolved_path);
                }
            }
        }
    }

    return m_ptr;
}

// Helper: load a .ctex compiled texture via the render API
static TextureHandle loadCompiledTexture(IRenderAPI* render_api, const std::string& ctex_path)
{
    Assets::CompiledTextureData tex_data;
    if (!Assets::CompiledTextureSerializer::load(tex_data, ctex_path))
        return INVALID_TEXTURE;

    std::vector<const uint8_t*> mip_ptrs;
    std::vector<size_t> mip_sizes;
    std::vector<std::pair<int,int>> mip_dims;

    for (const auto& mip : tex_data.mip_levels) {
        mip_ptrs.push_back(mip.data.data());
        mip_sizes.push_back(mip.data.size());
        mip_dims.push_back({static_cast<int>(mip.width), static_cast<int>(mip.height)});
    }

    return render_api->loadCompressedTexture(
        static_cast<int>(tex_data.header.width),
        static_cast<int>(tex_data.header.height),
        static_cast<uint32_t>(tex_data.header.format),
        static_cast<int>(tex_data.header.mip_count),
        mip_ptrs, mip_sizes, mip_dims);
}

// Helper: load texture with .ctex fallback
static TextureHandle loadTextureWithFallback(IRenderAPI* render_api, const std::string& path)
{
    // Try .ctex first
    std::filesystem::path p(path);
    std::string ctex_path = (p.parent_path() / p.stem()).string() + ".ctex";
    if (std::filesystem::exists(ctex_path)) {
        TextureHandle h = loadCompiledTexture(render_api, ctex_path);
        if (h != INVALID_TEXTURE) return h;
    }
    // Fallback to original format
    return render_api->loadTexture(path, true, true);
}

std::shared_ptr<mesh> LevelManager::loadCompiledMesh(const std::string& cmesh_path, IRenderAPI* render_api)
{
    Assets::CompiledMeshData cmesh;
    if (!Assets::CompiledMeshSerializer::load(cmesh, cmesh_path)) {
        LOG_ENGINE_ERROR("Failed to load compiled mesh: {}", cmesh_path);
        return nullptr;
    }

    if (cmesh.lod_levels.empty() || cmesh.lod_levels[0].vertices.empty()) {
        LOG_ENGINE_ERROR("Compiled mesh has no LOD0 data: {}", cmesh_path);
        return nullptr;
    }

    // Create mesh from LOD0
    const auto& lod0 = cmesh.lod_levels[0];
    auto m_ptr = std::make_shared<mesh>(cmesh_path);

    // Upload LOD0 to GPU
    if (render_api && !lod0.vertices.empty()) {
        m_ptr->gpu_mesh = render_api->createMesh();
        if (m_ptr->gpu_mesh) {
            if (!lod0.indices.empty()) {
                m_ptr->gpu_mesh->uploadIndexedMeshData(
                    lod0.vertices.data(), lod0.vertices.size(),
                    lod0.indices.data(), lod0.indices.size());
            } else {
                m_ptr->gpu_mesh->uploadMeshData(lod0.vertices.data(), lod0.vertices.size());
            }
        }
    }

    // Set AABB from header
    m_ptr->aabb_min = glm::vec3(cmesh.header.aabb_min[0], cmesh.header.aabb_min[1], cmesh.header.aabb_min[2]);
    m_ptr->aabb_max = glm::vec3(cmesh.header.aabb_max[0], cmesh.header.aabb_max[1], cmesh.header.aabb_max[2]);
    m_ptr->bounds_computed = true;

    // Resolve material textures
    std::string mesh_dir = std::filesystem::path(cmesh_path).parent_path().string();
    if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
        mesh_dir += "/";

    if (!cmesh.material_refs.empty() && render_api) {
        // Build material ranges from submeshes and LOD0 submesh_ranges
        std::vector<MaterialRange> material_ranges;

        if (!lod0.submesh_ranges.empty()) {
            for (const auto& sr : lod0.submesh_ranges) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name;

                if (sr.submesh_id < cmesh.material_refs.size()) {
                    const auto& mat = cmesh.material_refs[sr.submesh_id];
                    mat_name = mat.name;
                    // Load first texture reference
                    for (const auto& tr : mat.textures) {
                        std::string tex_path = mesh_dir + tr.path;
                        tex = loadTextureWithFallback(render_api, tex_path);
                        if (tex != INVALID_TEXTURE) break;
                    }
                }

                material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
            }
        } else if (!cmesh.submeshes.empty()) {
            // Fallback: use submesh table with LOD0 vertex data
            size_t current = 0;
            for (const auto& sub : cmesh.submeshes) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name = sub.name;

                if (sub.material_index < cmesh.material_refs.size()) {
                    const auto& mat = cmesh.material_refs[sub.material_index];
                    mat_name = mat.name;
                    for (const auto& tr : mat.textures) {
                        std::string tex_path = mesh_dir + tr.path;
                        tex = loadTextureWithFallback(render_api, tex_path);
                        if (tex != INVALID_TEXTURE) break;
                    }
                }

                // We don't have per-submesh ranges without indexed info
                material_ranges.emplace_back(current, 0, tex, mat_name);
            }
        }

        if (!material_ranges.empty()) {
            m_ptr->setMaterialRanges(material_ranges);
        }
    }

    // Load LOD1+ levels
    for (size_t i = 1; i < cmesh.lod_levels.size(); ++i) {
        const auto& lod = cmesh.lod_levels[i];
        if (lod.vertices.empty()) continue;

        mesh::LODLevel level;
        level.screen_threshold = lod.screen_threshold;
        level.vertex_count = lod.vertices.size();
        level.index_count = lod.indices.size();

        if (render_api) {
            level.gpu_mesh = render_api->createMesh();
            if (level.gpu_mesh) {
                if (!lod.indices.empty()) {
                    level.gpu_mesh->uploadIndexedMeshData(
                        lod.vertices.data(), lod.vertices.size(),
                        lod.indices.data(), lod.indices.size());
                } else {
                    level.gpu_mesh->uploadMeshData(lod.vertices.data(), lod.vertices.size());
                }
            }
        }

        // Map submesh ranges to material textures
        if (!lod.submesh_ranges.empty() && m_ptr->uses_material_ranges) {
            for (const auto& sr : lod.submesh_ranges) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name;
                if (sr.submesh_id < m_ptr->material_ranges.size()) {
                    tex = m_ptr->material_ranges[sr.submesh_id].texture;
                    mat_name = m_ptr->material_ranges[sr.submesh_id].material_name;
                }
                level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
            }
        }

        m_ptr->lod_levels.push_back(std::move(level));
    }

    if (!m_ptr->lod_levels.empty()) {
        LOG_ENGINE_TRACE("Loaded {} LOD levels from compiled mesh {}", m_ptr->lod_levels.size(), cmesh_path);
    }

    LOG_ENGINE_TRACE("Loaded compiled mesh: {} ({} verts, {} LODs)",
                     cmesh_path, lod0.vertices.size(), cmesh.lod_levels.size());
    return m_ptr;
}

// ============================================================================
// Parallel level loading implementation
// ============================================================================

void LevelManager::preloadMeshCPU(MeshPreloadData& data)
{
    // This runs on a worker thread - NO GPU calls, NO render_api, NO ECS access

    const std::string& resolved_path = data.resolved_path;
    if (resolved_path.empty()) {
        data.success = false;
        data.error_message = "Empty mesh path";
        return;
    }

    // Check for compiled mesh (.cmesh) first
    {
        std::filesystem::path p(resolved_path);
        std::string cmesh_path = (p.parent_path() / p.stem()).string() + ".cmesh";
        if (std::filesystem::exists(cmesh_path))
        {
            data.type = MeshPreloadData::Type::Compiled;
            data.compiled_data = std::make_unique<Assets::CompiledMeshData>();
            if (!Assets::CompiledMeshSerializer::load(*data.compiled_data, cmesh_path))
            {
                data.success = false;
                data.error_message = "Failed to load compiled mesh: " + cmesh_path;
                return;
            }

            // Pre-load compiled textures (.ctex) for materials
            std::string mesh_dir = std::filesystem::path(cmesh_path).parent_path().string();
            if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
                mesh_dir += "/";

            for (const auto& mat_ref : data.compiled_data->material_refs)
            {
                for (const auto& tex_ref : mat_ref.textures)
                {
                    std::string tex_path = mesh_dir + tex_ref.path;
                    // Try .ctex variant
                    std::filesystem::path tp(tex_path);
                    std::string ctex_path = (tp.parent_path() / tp.stem()).string() + ".ctex";
                    if (std::filesystem::exists(ctex_path))
                    {
                        if (data.preloaded_textures.find(ctex_path) == data.preloaded_textures.end())
                        {
                            MeshPreloadData::PreloadedTexture pt;
                            pt.is_compiled = true;
                            pt.success = Assets::CompiledTextureSerializer::load(pt.compiled_tex, ctex_path);
                            data.preloaded_textures[ctex_path] = std::move(pt);
                        }
                    }
                }
            }

            data.success = true;
            return;
        }
    }

    // Check file type
    size_t path_len = resolved_path.size();
    bool is_gltf = (path_len >= 5 && resolved_path.substr(path_len - 5) == ".gltf") ||
                   (path_len >= 4 && resolved_path.substr(path_len - 4) == ".glb");

    if (is_gltf)
    {
        data.type = MeshPreloadData::Type::GLTF;

        // Load geometry only - no render_api needed, fully thread-safe
        GltfLoaderConfig gltf_config;
        gltf_config.verbose_logging = false;
        gltf_config.flip_uvs = true;
        gltf_config.generate_normals_if_missing = true;
        gltf_config.scale = 1.0f;

        data.gltf_geometry = std::make_unique<GltfLoadResult>(
            GltfLoader::loadGltfGeometry(resolved_path, gltf_config));

        if (!data.gltf_geometry->success)
        {
            data.success = false;
            data.error_message = "Failed to load glTF geometry: " + data.gltf_geometry->error_message;
            return;
        }
    }
    else
    {
        data.type = MeshPreloadData::Type::OBJ;

        // OBJ loading - pure CPU, thread-safe
        ObjLoaderConfig obj_config;
        obj_config.verbose_logging = false;
        obj_config.triangulate = true;

        data.obj_result = std::make_unique<ObjLoadResult>(
            ObjLoader::loadObj(resolved_path, obj_config));

        if (!data.obj_result->success)
        {
            data.success = false;
            data.error_message = "Failed to load OBJ: " + data.obj_result->error_message;
            return;
        }
    }

    // Load LOD metadata (applies to non-compiled GLTF/OBJ meshes)
    std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(resolved_path);
    data.lod_metadata = std::make_unique<Assets::AssetMetadata>();
    if (Assets::AssetMetadataSerializer::load(*data.lod_metadata, meta_path) &&
        data.lod_metadata->lod_enabled)
    {
        data.has_lod_metadata = true;

        std::string mesh_dir = std::filesystem::path(resolved_path).parent_path().string();
        if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
            mesh_dir += "/";

        for (size_t i = 1; i < data.lod_metadata->lod_levels.size(); ++i)
        {
            const auto& lod_info = data.lod_metadata->lod_levels[i];
            if (lod_info.file_path.empty()) continue;

            std::string lod_path = mesh_dir + lod_info.file_path;
            Assets::LODMeshData lod_data;
            if (Assets::LODMeshSerializer::load(lod_data, lod_path))
            {
                data.lod_mesh_data.push_back(std::move(lod_data));
            }
        }
    }

    data.success = true;
}

// Helper: upload a preloaded compiled texture to GPU (main thread)
static TextureHandle uploadPreloadedCompiledTexture(IRenderAPI* render_api,
                                                     const Assets::CompiledTextureData& tex_data)
{
    std::vector<const uint8_t*> mip_ptrs;
    std::vector<size_t> mip_sizes;
    std::vector<std::pair<int,int>> mip_dims;

    for (const auto& mip : tex_data.mip_levels) {
        mip_ptrs.push_back(mip.data.data());
        mip_sizes.push_back(mip.data.size());
        mip_dims.push_back({static_cast<int>(mip.width), static_cast<int>(mip.height)});
    }

    return render_api->loadCompressedTexture(
        static_cast<int>(tex_data.header.width),
        static_cast<int>(tex_data.header.height),
        static_cast<uint32_t>(tex_data.header.format),
        static_cast<int>(tex_data.header.mip_count),
        mip_ptrs, mip_sizes, mip_dims);
}

std::shared_ptr<mesh> LevelManager::finalizeCompiledMeshGPU(
    MeshPreloadData& preload,
    const LevelEntity& entity,
    IRenderAPI* render_api)
{
    // Main thread only - does GPU uploads using pre-loaded CPU data
    auto& cmesh = *preload.compiled_data;

    if (cmesh.lod_levels.empty() || cmesh.lod_levels[0].vertices.empty()) {
        LOG_ENGINE_ERROR("Compiled mesh has no LOD0 data: {}", preload.resolved_path);
        return nullptr;
    }

    const auto& lod0 = cmesh.lod_levels[0];
    auto m_ptr = std::make_shared<mesh>(preload.resolved_path);

    // Upload LOD0 to GPU
    if (render_api && !lod0.vertices.empty()) {
        m_ptr->gpu_mesh = render_api->createMesh();
        if (m_ptr->gpu_mesh) {
            if (!lod0.indices.empty()) {
                m_ptr->gpu_mesh->uploadIndexedMeshData(
                    lod0.vertices.data(), lod0.vertices.size(),
                    lod0.indices.data(), lod0.indices.size());
            } else {
                m_ptr->gpu_mesh->uploadMeshData(lod0.vertices.data(), lod0.vertices.size());
            }
        }
    }

    // Set AABB from header
    m_ptr->aabb_min = glm::vec3(cmesh.header.aabb_min[0], cmesh.header.aabb_min[1], cmesh.header.aabb_min[2]);
    m_ptr->aabb_max = glm::vec3(cmesh.header.aabb_max[0], cmesh.header.aabb_max[1], cmesh.header.aabb_max[2]);
    m_ptr->bounds_computed = true;

    // Resolve material textures using preloaded texture data
    std::string mesh_dir = std::filesystem::path(preload.resolved_path).parent_path().string();
    if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
        mesh_dir += "/";

    if (!cmesh.material_refs.empty() && render_api) {
        std::vector<MaterialRange> material_ranges;

        if (!lod0.submesh_ranges.empty()) {
            for (const auto& sr : lod0.submesh_ranges) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name;

                if (sr.submesh_id < cmesh.material_refs.size()) {
                    const auto& mat = cmesh.material_refs[sr.submesh_id];
                    mat_name = mat.name;
                    for (const auto& tr : mat.textures) {
                        std::string tex_path = mesh_dir + tr.path;
                        // Try preloaded .ctex first
                        std::filesystem::path tp(tex_path);
                        std::string ctex_path = (tp.parent_path() / tp.stem()).string() + ".ctex";
                        auto it = preload.preloaded_textures.find(ctex_path);
                        if (it != preload.preloaded_textures.end() && it->second.success) {
                            tex = uploadPreloadedCompiledTexture(render_api, it->second.compiled_tex);
                        } else {
                            // Fallback to loading from disk (main thread)
                            tex = loadTextureWithFallback(render_api, tex_path);
                        }
                        if (tex != INVALID_TEXTURE) break;
                    }
                }

                material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
            }
        } else if (!cmesh.submeshes.empty()) {
            size_t current = 0;
            for (const auto& sub : cmesh.submeshes) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name = sub.name;

                if (sub.material_index < cmesh.material_refs.size()) {
                    const auto& mat = cmesh.material_refs[sub.material_index];
                    mat_name = mat.name;
                    for (const auto& tr : mat.textures) {
                        std::string tex_path = mesh_dir + tr.path;
                        std::filesystem::path tp(tex_path);
                        std::string ctex_path = (tp.parent_path() / tp.stem()).string() + ".ctex";
                        auto it = preload.preloaded_textures.find(ctex_path);
                        if (it != preload.preloaded_textures.end() && it->second.success) {
                            tex = uploadPreloadedCompiledTexture(render_api, it->second.compiled_tex);
                        } else {
                            tex = loadTextureWithFallback(render_api, tex_path);
                        }
                        if (tex != INVALID_TEXTURE) break;
                    }
                }

                material_ranges.emplace_back(current, 0, tex, mat_name);
            }
        }

        if (!material_ranges.empty()) {
            m_ptr->setMaterialRanges(material_ranges);
        }
    }

    // Load LOD1+ levels to GPU
    for (size_t i = 1; i < cmesh.lod_levels.size(); ++i) {
        const auto& lod = cmesh.lod_levels[i];
        if (lod.vertices.empty()) continue;

        mesh::LODLevel level;
        level.screen_threshold = lod.screen_threshold;
        level.vertex_count = lod.vertices.size();
        level.index_count = lod.indices.size();

        if (render_api) {
            level.gpu_mesh = render_api->createMesh();
            if (level.gpu_mesh) {
                if (!lod.indices.empty()) {
                    level.gpu_mesh->uploadIndexedMeshData(
                        lod.vertices.data(), lod.vertices.size(),
                        lod.indices.data(), lod.indices.size());
                } else {
                    level.gpu_mesh->uploadMeshData(lod.vertices.data(), lod.vertices.size());
                }
            }
        }

        if (!lod.submesh_ranges.empty() && m_ptr->uses_material_ranges) {
            for (const auto& sr : lod.submesh_ranges) {
                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name;
                if (sr.submesh_id < m_ptr->material_ranges.size()) {
                    tex = m_ptr->material_ranges[sr.submesh_id].texture;
                    mat_name = m_ptr->material_ranges[sr.submesh_id].material_name;
                }
                level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
            }
        }

        m_ptr->lod_levels.push_back(std::move(level));
    }

    if (!m_ptr->lod_levels.empty()) {
        LOG_ENGINE_TRACE("Loaded {} LOD levels from compiled mesh {}", m_ptr->lod_levels.size(), preload.resolved_path);
    }

    // Apply entity properties
    m_ptr->culling = entity.culling;
    m_ptr->transparent = entity.transparent;
    m_ptr->visible = entity.visible;
    m_ptr->casts_shadow = entity.casts_shadow;
    m_ptr->force_lod = entity.force_lod;

    return m_ptr;
}

std::shared_ptr<mesh> LevelManager::finalizeMeshGPU(
    MeshPreloadData& preload,
    const LevelEntity& entity,
    IRenderAPI* render_api)
{
    // Main thread only - takes pre-loaded CPU data and does GPU uploads
    if (!preload.success) return nullptr;

    // Compiled mesh path
    if (preload.type == MeshPreloadData::Type::Compiled) {
        return finalizeCompiledMeshGPU(preload, entity, render_api);
    }

    std::shared_ptr<mesh> m_ptr;

    if (preload.type == MeshPreloadData::Type::GLTF)
    {
        auto& gltf = *preload.gltf_geometry;

        // Create mesh from preloaded geometry
        m_ptr = std::make_shared<mesh>(gltf.vertices, gltf.vertex_count);
        // Transfer ownership
        gltf.vertices = nullptr;
        gltf.vertex_count = 0;

        // Now load materials on main thread (requires render_api for texture uploads)
        MaterialLoaderConfig material_config;
        material_config.verbose_logging = false;
        material_config.load_all_textures = false;
        material_config.priority_texture_types = {
            TextureType::BASE_COLOR,
            TextureType::DIFFUSE,
            TextureType::NORMAL
        };
        material_config.generate_mipmaps = true;
        material_config.flip_textures_vertically = true;
        material_config.cache_textures = true;
        {
            size_t last_sep = preload.resolved_path.find_last_of("/\\");
            material_config.texture_base_path = (last_sep != std::string::npos)
                ? preload.resolved_path.substr(0, last_sep + 1)
                : "";
        }

        // Load materials into the existing geometry result (main thread GPU calls)
        GltfLoader::loadMaterialsIntoResult(gltf, preload.resolved_path, render_api, material_config);

        // Apply textures from materials
        bool texture_applied = false;
        if (gltf.materials_loaded && !gltf.material_data.materials.empty()) {
            std::vector<MaterialRange> material_ranges;
            size_t current_vertex = 0;

            for (size_t i = 0; i < gltf.material_indices.size(); ++i) {
                int mat_idx = gltf.material_indices[i];
                size_t vertex_count = gltf.primitive_vertex_counts[i];

                if (mat_idx >= 0 && mat_idx < (int)gltf.material_data.materials.size()) {
                    const auto& material = gltf.material_data.materials[mat_idx];
                    TextureHandle tex = material.getPrimaryTextureHandle();
                    MaterialRange range(current_vertex, vertex_count, tex, material.properties.name);
                    material_ranges.push_back(range);
                    if (tex != INVALID_TEXTURE) texture_applied = true;
                } else {
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
        if (!texture_applied && !entity.texture_paths.empty() && render_api) {
            std::string tex_resolved = Assets::AssetManager::get().resolveAssetPath(entity.texture_paths[0]);
            TextureHandle tex = render_api->loadTexture(tex_resolved, true, true);
            m_ptr->set_texture(tex);
        }
    }
    else if (preload.type == MeshPreloadData::Type::OBJ)
    {
        auto& obj = *preload.obj_result;

        // Create mesh from preloaded OBJ data
        m_ptr = std::make_shared<mesh>(obj.vertices, obj.vertex_count);
        // Transfer ownership
        obj.vertices = nullptr;
        obj.vertex_count = 0;

        // Load textures (main thread GPU call)
        if (!entity.texture_paths.empty() && render_api) {
            for (const auto& tex_path : entity.texture_paths) {
                std::string tex_resolved = Assets::AssetManager::get().resolveAssetPath(tex_path);
                TextureHandle tex = render_api->loadTexture(tex_resolved, true, true);
                m_ptr->set_texture(tex);
                break; // Use first texture
            }
        }
    }

    if (!m_ptr) return nullptr;

    // Apply entity properties
    m_ptr->culling = entity.culling;
    m_ptr->transparent = entity.transparent;
    m_ptr->visible = entity.visible;
    m_ptr->casts_shadow = entity.casts_shadow;
    m_ptr->force_lod = entity.force_lod;

    // Upload mesh to GPU
    if (render_api && m_ptr->is_valid && !m_ptr->isUploadedToGPU()) {
        m_ptr->uploadToGPU(render_api);
    }

    // Finalize LOD data (upload to GPU)
    if (preload.has_lod_metadata && render_api && !preload.lod_mesh_data.empty())
    {
        size_t lod_data_idx = 0;
        for (size_t i = 1; i < preload.lod_metadata->lod_levels.size() && lod_data_idx < preload.lod_mesh_data.size(); ++i)
        {
            const auto& lod_info = preload.lod_metadata->lod_levels[i];
            if (lod_info.file_path.empty()) continue;

            auto& lod_data = preload.lod_mesh_data[lod_data_idx++];

            mesh::LODLevel level;
            level.screen_threshold = lod_info.screen_threshold;
            level.vertex_count = lod_data.vertices.size();
            level.index_count = lod_data.indices.size();
            level.gpu_mesh = render_api->createMesh();
            if (level.gpu_mesh) {
                level.gpu_mesh->uploadIndexedMeshData(
                    lod_data.vertices.data(), lod_data.vertices.size(),
                    lod_data.indices.data(), lod_data.indices.size());
            }

            // Map LOD submesh ranges to original mesh's material textures
            if (!lod_data.submesh_ranges.empty() && m_ptr->uses_material_ranges) {
                for (const auto& sr : lod_data.submesh_ranges) {
                    TextureHandle tex = INVALID_TEXTURE;
                    std::string mat_name = "";
                    if (sr.submesh_id < m_ptr->material_ranges.size()) {
                        tex = m_ptr->material_ranges[sr.submesh_id].texture;
                        mat_name = m_ptr->material_ranges[sr.submesh_id].material_name;
                    }
                    level.material_ranges.emplace_back(sr.start_index, sr.index_count, tex, mat_name);
                }
            }

            m_ptr->lod_levels.push_back(std::move(level));
        }

        if (!m_ptr->lod_levels.empty()) {
            m_ptr->computeBounds();
            LOG_ENGINE_TRACE("Loaded {} LOD levels for {}", m_ptr->lod_levels.size(), preload.resolved_path);
        }
    }

    return m_ptr;
}

bool LevelManager::instantiateLevelParallel(
    const LevelData& level_data,
    world& game_world,
    IRenderAPI* render_api,
    entt::entity* out_player_entity,
    entt::entity* out_freecam_entity,
    entt::entity* out_player_rep_entity)
{
    LOG_ENGINE_INFO("Instantiating level (parallel): {}", level_data.metadata.level_name);

    // ========================================================================
    // PHASE 1: SCAN - Collect unique mesh paths (main thread, fast)
    // ========================================================================
    std::unordered_map<std::string, std::unique_ptr<MeshPreloadData>> preload_cache;

    auto tryInsertPath = [&](const std::string& mesh_path) {
        if (mesh_path.empty()) return;
        std::string resolved = Assets::AssetManager::get().resolveAssetPath(mesh_path);
        if (preload_cache.find(resolved) == preload_cache.end()) {
            auto data = std::make_unique<MeshPreloadData>();
            data->resolved_path = resolved;
            preload_cache[resolved] = std::move(data);
        }
    };

    for (const auto& entity_data : level_data.entities)
    {
        tryInsertPath(entity_data.mesh_path);
        if (!entity_data.collider_mesh_path.empty())
            tryInsertPath(entity_data.collider_mesh_path);
    }

    LOG_ENGINE_INFO("Phase 1 complete: {} unique mesh paths to preload", preload_cache.size());

    // ========================================================================
    // PHASE 2: PARALLEL PRELOAD - Worker threads load files to CPU memory
    // ========================================================================
    if (!preload_cache.empty() && Threading::JobSystem::get().isInitialized())
    {
        std::vector<Threading::JobHandle> preload_jobs;
        preload_jobs.reserve(preload_cache.size());

        for (auto& [path, preload] : preload_cache)
        {
            MeshPreloadData* ptr = preload.get();
            auto handle = Threading::JobSystem::get().createJob()
                .setName("PreloadMesh")
                .setWork([this, ptr]() { preloadMeshCPU(*ptr); })
                .setPriority(Threading::JobPriority::High)
                .setContext(Threading::JobContext::Worker)
                .submit();
            preload_jobs.push_back(handle);
        }

        // Wait for all preload jobs to finish
        Threading::JobSystem::get().waitForJobs(preload_jobs);

        LOG_ENGINE_INFO("Phase 2 complete: all mesh preloads finished");
    }
    else if (!preload_cache.empty())
    {
        // Fallback: load sequentially if job system not initialized
        for (auto& [path, preload] : preload_cache) {
            preloadMeshCPU(*preload);
        }
    }

    // ========================================================================
    // PHASE 3: SEQUENTIAL FINALIZE - Main thread: GPU uploads, ECS, physics
    // ========================================================================

    // Apply world settings
    game_world.setGravity(level_data.metadata.gravity);
    game_world.setFixedDelta(level_data.metadata.fixed_delta);

    // Initialize output pointers
    if (out_player_entity) *out_player_entity = entt::null;
    if (out_freecam_entity) *out_freecam_entity = entt::null;
    if (out_player_rep_entity) *out_player_rep_entity = entt::null;

    // Map to store entities by name for reference resolution
    std::map<std::string, entt::entity> entity_map;
    std::vector<entt::entity> created_entities;
    created_entities.reserve(level_data.entities.size());

    // Cache finalized meshes to avoid duplicate GPU uploads for same path
    std::unordered_map<std::string, std::shared_ptr<mesh>> finalized_meshes;

    // Helper to get or finalize a mesh from the preload cache
    auto getOrFinalizeMesh = [&](const std::string& mesh_path, const LevelEntity& ent) -> std::shared_ptr<mesh> {
        if (mesh_path.empty()) return nullptr;
        std::string resolved = Assets::AssetManager::get().resolveAssetPath(mesh_path);

        // Check if already finalized (GPU-level dedup)
        auto fin_it = finalized_meshes.find(resolved);
        if (fin_it != finalized_meshes.end()) {
            return fin_it->second;
        }

        // Look up preloaded data
        auto pre_it = preload_cache.find(resolved);
        if (pre_it == preload_cache.end() || !pre_it->second->success) {
            if (pre_it != preload_cache.end()) {
                LOG_ENGINE_ERROR("Preload failed for {}: {}", resolved, pre_it->second->error_message);
            }
            return nullptr;
        }

        auto mesh_ptr = finalizeMeshGPU(*pre_it->second, ent, render_api);
        if (mesh_ptr) {
            finalized_meshes[resolved] = mesh_ptr;
        }
        return mesh_ptr;
    };

    // Create all entities (same logic as instantiateLevel, using preloaded data)
    for (const auto& entity_data : level_data.entities)
    {
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

        // Load and add Mesh (using preloaded data)
        if (entity_data.type == EntityType::Renderable ||
            entity_data.type == EntityType::Physical ||
            entity_data.type == EntityType::PlayerRep)
        {
            if (!entity_data.mesh_path.empty()) {
                auto mesh_ptr = getOrFinalizeMesh(entity_data.mesh_path, entity_data);
                if (mesh_ptr) {
                    game_world.registry.emplace<MeshComponent>(e, mesh_ptr);
                }
            }
        }

        // Renderable with explicit collider
        if (entity_data.type == EntityType::Renderable && entity_data.has_collider)
        {
            game_world.registry.emplace<ColliderComponent>(e);
            auto& col = game_world.registry.get<ColliderComponent>(e);

            col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
            col.box_half_extents = entity_data.collider_box_half_extents;
            col.sphere_radius = entity_data.collider_sphere_radius;
            col.capsule_half_height = entity_data.collider_capsule_half_height;
            col.capsule_radius = entity_data.collider_capsule_radius;
            col.cylinder_half_height = entity_data.collider_cylinder_half_height;
            col.cylinder_radius = entity_data.collider_cylinder_radius;
            col.friction = entity_data.collider_friction;
            col.restitution = entity_data.collider_restitution;

            if (!entity_data.collider_mesh_path.empty()) {
                LevelEntity col_ent = entity_data;
                col_ent.mesh_path = entity_data.collider_mesh_path;
                col_ent.texture_paths.clear();
                col.m_mesh = getOrFinalizeMesh(entity_data.collider_mesh_path, col_ent);
            } else if (game_world.registry.all_of<MeshComponent>(e)) {
                col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
            }
        }

        // Add Physics components
        if (entity_data.type == EntityType::Physical ||
            entity_data.type == EntityType::Player)
        {
            if (entity_data.has_rigidbody) {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = entity_data.mass;
                rb.apply_gravity = entity_data.apply_gravity;
                rb.motion_type = stringToBodyMotionType(entity_data.body_motion_type);
            }

            if (entity_data.has_collider) {
                game_world.registry.emplace<ColliderComponent>(e);
                auto& col = game_world.registry.get<ColliderComponent>(e);

                col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
                col.box_half_extents = entity_data.collider_box_half_extents;
                col.sphere_radius = entity_data.collider_sphere_radius;
                col.capsule_half_height = entity_data.collider_capsule_half_height;
                col.capsule_radius = entity_data.collider_capsule_radius;
                col.cylinder_half_height = entity_data.collider_cylinder_half_height;
                col.cylinder_radius = entity_data.collider_cylinder_radius;
                col.friction = entity_data.collider_friction;
                col.restitution = entity_data.collider_restitution;

                if (!entity_data.collider_mesh_path.empty()) {
                    LevelEntity col_ent = entity_data;
                    col_ent.mesh_path = entity_data.collider_mesh_path;
                    col_ent.texture_paths.clear();
                    col.m_mesh = getOrFinalizeMesh(entity_data.collider_mesh_path, col_ent);
                } else if (game_world.registry.all_of<MeshComponent>(e)) {
                    col.m_mesh = game_world.registry.get<MeshComponent>(e).m_mesh;
                }
            }
        }

        // Collidable only (static collider)
        if (entity_data.type == EntityType::Collidable) {
            if (entity_data.has_collider) {
                game_world.registry.emplace<ColliderComponent>(e);
                auto& col = game_world.registry.get<ColliderComponent>(e);

                col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
                col.box_half_extents = entity_data.collider_box_half_extents;
                col.sphere_radius = entity_data.collider_sphere_radius;
                col.capsule_half_height = entity_data.collider_capsule_half_height;
                col.capsule_radius = entity_data.collider_capsule_radius;
                col.cylinder_half_height = entity_data.collider_cylinder_half_height;
                col.cylinder_radius = entity_data.collider_cylinder_radius;
                col.friction = entity_data.collider_friction;
                col.restitution = entity_data.collider_restitution;

                if (!entity_data.collider_mesh_path.empty()) {
                    LevelEntity col_ent = entity_data;
                    col_ent.mesh_path = entity_data.collider_mesh_path;
                    col.m_mesh = getOrFinalizeMesh(entity_data.collider_mesh_path, col_ent);
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
        }

        // Register collider shapes with Jolt physics
        if (game_world.registry.all_of<ColliderComponent>(e))
        {
            auto& col = game_world.registry.get<ColliderComponent>(e);
            auto& t = game_world.registry.get<TransformComponent>(e);

            if (entity_data.type == EntityType::Physical)
            {
                BodyMotionType motion = BodyMotionType::Dynamic;
                if (game_world.registry.all_of<RigidBodyComponent>(e))
                    motion = game_world.registry.get<RigidBodyComponent>(e).motion_type;

                if (motion == BodyMotionType::Kinematic)
                {
                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (!shape && col.shape_type == ColliderShapeType::Mesh && col.is_mesh_valid()) {
                        game_world.getPhysicsSystem().createStaticMeshBody(
                            t.position, t.rotation, t.scale, *col.get_mesh(), e);
                    } else if (shape) {
                        game_world.getPhysicsSystem().createKinematicBody(
                            t.position, t.rotation, shape, e);
                    }
                }
                else
                {
                    if (col.shape_type == ColliderShapeType::Mesh) {
                        if (col.is_mesh_valid()) {
                            col.shape_type = ColliderShapeType::ConvexHull;
                        }
                    }
                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (shape) {
                        game_world.getPhysicsSystem().createDynamicBody(
                            t.position, t.rotation, shape, entity_data.mass, e,
                            col.friction, col.restitution);
                    }
                }
            }
            else
            {
                if (col.shape_type == ColliderShapeType::Mesh && col.is_mesh_valid()) {
                    game_world.getPhysicsSystem().createStaticMeshBody(
                        t.position, t.rotation, t.scale, *col.get_mesh(), e);
                } else {
                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (shape) {
                        game_world.getPhysicsSystem().createStaticBody(
                            t.position, t.rotation, shape, e);
                    }
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

            if (!game_world.registry.all_of<RigidBodyComponent>(e)) {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = 80.0f;
                rb.apply_gravity = true;
            }

            {
                auto& t = game_world.registry.get<TransformComponent>(e);
                auto& pc2 = game_world.registry.get<PlayerComponent>(e);
                JPH::CapsuleShapeSettings capsule(pc2.capsule_half_height, pc2.capsule_radius);
                auto shape_result = capsule.Create();
                if (shape_result.IsValid()) {
                    game_world.getPhysicsSystem().createDynamicBody(
                        t.position, t.rotation, shape_result.Get(), 80.0f, e);
                }
            }

            if (out_player_entity) *out_player_entity = e;
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
        }

        // Player Rep
        if (entity_data.type == EntityType::PlayerRep)
        {
            game_world.registry.emplace<PlayerRepresentationComponent>(e);
            auto& pr = game_world.registry.get<PlayerRepresentationComponent>(e);
            pr.position_offset = entity_data.position_offset;

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

        // Constraint (data only - resolved in third pass)
        if (entity_data.has_constraint)
        {
            auto& cc = game_world.registry.emplace<ConstraintComponent>(e);
            cc.type = stringToConstraintType(entity_data.constraint_type);
            cc.target_entity_name = entity_data.constraint_target_name;
            cc.anchor_1 = entity_data.constraint_anchor_1;
            cc.anchor_2 = entity_data.constraint_anchor_2;
            cc.hinge_axis = entity_data.constraint_hinge_axis;
            cc.hinge_min_limit = entity_data.constraint_hinge_min;
            cc.hinge_max_limit = entity_data.constraint_hinge_max;
            cc.min_distance = entity_data.constraint_min_distance;
            cc.max_distance = entity_data.constraint_max_distance;
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
                    LOG_ENGINE_WARN("PlayerRepresentation '{}' cannot find tracked player '{}'",
                                    entity_data.name, entity_data.tracked_player_name);
                }
            }
        }
    }

    // Third pass: Create constraints (requires both bodies to exist)
    for (size_t i = 0; i < level_data.entities.size(); ++i) {
        const auto& entity_data = level_data.entities[i];
        if (entity_data.has_constraint) {
            entt::entity e = created_entities[i];
            if (game_world.registry.all_of<ConstraintComponent>(e)) {
                auto& cc = game_world.registry.get<ConstraintComponent>(e);
                auto target_it = entity_map.find(entity_data.constraint_target_name);
                if (target_it != entity_map.end()) {
                    cc.target_entity = target_it->second;
                    game_world.getPhysicsSystem().createConstraint(e, cc.target_entity, cc);
                } else {
                    LOG_ENGINE_WARN("Constraint on '{}' cannot find target '{}'",
                                    entity_data.name, entity_data.constraint_target_name);
                }
            }
        }
    }

    LOG_ENGINE_INFO("Level instantiation complete (parallel): {} entities", level_data.entities.size());
    return true;
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

            // Copy shape parameters
            col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
            col.box_half_extents = entity_data.collider_box_half_extents;
            col.sphere_radius = entity_data.collider_sphere_radius;
            col.capsule_half_height = entity_data.collider_capsule_half_height;
            col.capsule_radius = entity_data.collider_capsule_radius;
            col.cylinder_half_height = entity_data.collider_cylinder_half_height;
            col.cylinder_radius = entity_data.collider_cylinder_radius;
            col.friction = entity_data.collider_friction;
            col.restitution = entity_data.collider_restitution;

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
                rb.motion_type = stringToBodyMotionType(entity_data.body_motion_type);
            }

            // Collider
            if (entity_data.has_collider) {
                game_world.registry.emplace<ColliderComponent>(e);
                auto& col = game_world.registry.get<ColliderComponent>(e);

                // Copy shape parameters from level data
                col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
                col.box_half_extents = entity_data.collider_box_half_extents;
                col.sphere_radius = entity_data.collider_sphere_radius;
                col.capsule_half_height = entity_data.collider_capsule_half_height;
                col.capsule_radius = entity_data.collider_capsule_radius;
                col.cylinder_half_height = entity_data.collider_cylinder_half_height;
                col.cylinder_radius = entity_data.collider_cylinder_radius;
                col.friction = entity_data.collider_friction;
                col.restitution = entity_data.collider_restitution;

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

                // Copy shape parameters
                col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
                col.box_half_extents = entity_data.collider_box_half_extents;
                col.sphere_radius = entity_data.collider_sphere_radius;
                col.capsule_half_height = entity_data.collider_capsule_half_height;
                col.capsule_radius = entity_data.collider_capsule_radius;
                col.cylinder_half_height = entity_data.collider_cylinder_half_height;
                col.cylinder_radius = entity_data.collider_cylinder_radius;
                col.friction = entity_data.collider_friction;
                col.restitution = entity_data.collider_restitution;

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

        // Register collider shapes with Jolt physics
        if (game_world.registry.all_of<ColliderComponent>(e))
        {
            auto& col = game_world.registry.get<ColliderComponent>(e);
            auto& t = game_world.registry.get<TransformComponent>(e);
            LOG_ENGINE_INFO("Jolt: entity '{}' has ColliderComponent, shape_type={}, mesh_valid={}",
                entity_data.name, colliderShapeTypeToString(col.shape_type), col.is_mesh_valid());

            if (entity_data.type == EntityType::Physical)
            {
                // Determine motion type from RigidBodyComponent
                BodyMotionType motion = BodyMotionType::Dynamic;
                if (game_world.registry.all_of<RigidBodyComponent>(e))
                    motion = game_world.registry.get<RigidBodyComponent>(e).motion_type;

                if (motion == BodyMotionType::Kinematic)
                {
                    // Kinematic body
                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (!shape && col.shape_type == ColliderShapeType::Mesh && col.is_mesh_valid())
                    {
                        // Use mesh as static for kinematic (rare case)
                        game_world.getPhysicsSystem().createStaticMeshBody(
                            t.position, t.rotation, t.scale, *col.get_mesh(), e);
                    }
                    else if (shape)
                    {
                        game_world.getPhysicsSystem().createKinematicBody(
                            t.position, t.rotation, shape, e);
                    }
                }
                else
                {
                    // Dynamic body: use shape from ColliderComponent
                    if (col.shape_type == ColliderShapeType::Mesh)
                    {
                        // Mesh shapes cannot be dynamic in Jolt; fall back to ConvexHull
                        if (col.is_mesh_valid()) {
                            col.shape_type = ColliderShapeType::ConvexHull;
                            LOG_ENGINE_WARN("Entity '{}': Mesh collider on dynamic body not supported, using ConvexHull", entity_data.name);
                        }
                    }

                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (shape)
                    {
                        game_world.getPhysicsSystem().createDynamicBody(
                            t.position, t.rotation, shape, entity_data.mass, e,
                            col.friction, col.restitution);
                    }
                }
            }
            else
            {
                // Static body
                if (col.shape_type == ColliderShapeType::Mesh && col.is_mesh_valid())
                {
                    // Static mesh collider (apply entity scale)
                    game_world.getPhysicsSystem().createStaticMeshBody(
                        t.position, t.rotation, t.scale, *col.get_mesh(), e);
                }
                else
                {
                    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
                    if (shape)
                    {
                        game_world.getPhysicsSystem().createStaticBody(
                            t.position, t.rotation, shape, e);
                    }
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
        // Constraint (data only — resolved in third pass)
        if (entity_data.has_constraint)
        {
            auto& cc = game_world.registry.emplace<ConstraintComponent>(e);
            cc.type = stringToConstraintType(entity_data.constraint_type);
            cc.target_entity_name = entity_data.constraint_target_name;
            cc.anchor_1 = entity_data.constraint_anchor_1;
            cc.anchor_2 = entity_data.constraint_anchor_2;
            cc.hinge_axis = entity_data.constraint_hinge_axis;
            cc.hinge_min_limit = entity_data.constraint_hinge_min;
            cc.hinge_max_limit = entity_data.constraint_hinge_max;
            cc.min_distance = entity_data.constraint_min_distance;
            cc.max_distance = entity_data.constraint_max_distance;
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

    // Third pass: Create constraints (requires both bodies to exist)
    for (size_t i = 0; i < level_data.entities.size(); ++i) {
        const auto& entity_data = level_data.entities[i];
        if (entity_data.has_constraint) {
            entt::entity e = created_entities[i];
            if (game_world.registry.all_of<ConstraintComponent>(e)) {
                auto& cc = game_world.registry.get<ConstraintComponent>(e);
                auto target_it = entity_map.find(entity_data.constraint_target_name);
                if (target_it != entity_map.end()) {
                    cc.target_entity = target_it->second;
                    game_world.getPhysicsSystem().createConstraint(e, cc.target_entity, cc);
                } else {
                    printf("WARNING: Constraint on '%s' cannot find target '%s'\n",
                           entity_data.name.c_str(), entity_data.constraint_target_name.c_str());
                }
            }
        }
    }

    printf("Level instantiation complete: %d entities\n", (int)level_data.entities.size());
    return true;
}