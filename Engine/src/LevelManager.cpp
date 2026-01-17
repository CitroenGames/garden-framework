#include "LevelManager.hpp"
#include "Thirdparty/tinygltf-2.9.6/json.hpp"
#include "Components/Components.hpp"
#include "world.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/GltfMaterialLoader.hpp"
#include "Utils/Log.hpp"
#include <iostream>
#include <cstring>

using json = nlohmann::json;

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
            metadata.gravity = vector3f(
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
            metadata.ambient_light = vector3f(
                a.value("r", 0.2f),
                a.value("g", 0.2f),
                a.value("b", 0.2f)
            );
        }
        if (lighting.contains("diffuse"))
        {
            const auto& d = lighting["diffuse"];
            metadata.diffuse_light = vector3f(
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
            metadata.light_direction = vector3f(x, y, z);
            // Normalize direction
            float length = std::sqrt(x*x + y*y + z*z);
            if (length > 0.0001f) {
                metadata.light_direction.X /= length;
                metadata.light_direction.Y /= length;
                metadata.light_direction.Z /= length;
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
            metadata.light_direction = vector3f(-x, -y, -z);
            // Normalize
            float length = std::sqrt(x*x + y*y + z*z);
            if (length > 0.0001f) {
                metadata.light_direction.X /= length;
                metadata.light_direction.Y /= length;
                metadata.light_direction.Z /= length;
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
    }

    // Parse transform
    if (j.contains("transform"))
    {
        const auto& transform = j["transform"];
        if (transform.contains("position"))
        {
            const auto& pos = transform["position"];
            entity.position = vector3f(
                pos.value("x", 0.0f),
                pos.value("y", 0.0f),
                pos.value("z", 0.0f)
            );
        }
        if (transform.contains("rotation"))
        {
            const auto& rot = transform["rotation"];
            entity.rotation = vector3f(
                rot.value("x", 0.0f),
                rot.value("y", 0.0f),
                rot.value("z", 0.0f)
            );
        }
        if (transform.contains("scale"))
        {
            const auto& scl = transform["scale"];
            entity.scale = vector3f(
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
            entity.position_offset = vector3f(
                offset.value("x", 0.0f),
                offset.value("y", 0.0f),
                offset.value("z", 0.0f)
            );
        }
    }

    return true;
}

bool LevelManager::saveLevelToJSON(const std::string& json_path, const LevelData& level_data)
{
    printf("JSON saving not yet implemented\n");
    return false;
}

bool LevelManager::loadLevelFromBinary(const std::string& binary_path, LevelData& out_level_data)
{
    printf("Binary loading not yet implemented\n");
    return false;
}

bool LevelManager::saveLevelToBinary(const std::string& binary_path, const LevelData& level_data)
{
    printf("Binary saving not yet implemented\n");
    return false;
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

std::shared_ptr<mesh> LevelManager::loadMesh(const LevelEntity& entity, IRenderAPI* render_api)
{
    if (entity.mesh_path.empty())
    {
        return nullptr;
    }

    mesh* m_ptr = nullptr;

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
        material_config.texture_base_path = "models/";

        // Load geometry and materials
        GltfLoadResult map_result = GltfLoader::loadGltfWithMaterials(entity.mesh_path, render_api, gltf_config, material_config);

        if (!map_result.success) {
            LOG_ENGINE_FATAL("Failed to load glTF file: %s\n", map_result.error_message.c_str());
            return nullptr;
        }

        LOG_ENGINE_TRACE("Loaded glTF: {}", entity.mesh_path.c_str());
        
        // Create mesh from glTF data
        m_ptr = new mesh(map_result.vertices, map_result.vertex_count);

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
        m_ptr = new mesh(entity.mesh_path);
        
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
        return std::shared_ptr<mesh>(m_ptr);
    }

    return nullptr;
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
        game_world.registry.emplace<TransformComponent>(e, entity_data.position.X, entity_data.position.Y, entity_data.position.Z);
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

        // Player
        if (entity_data.type == EntityType::Player)
        {
            game_world.registry.emplace<PlayerComponent>(e);
            auto& pc = game_world.registry.get<PlayerComponent>(e);
            pc.speed = entity_data.speed;
            pc.jump_force = entity_data.jump_force;
            pc.mouse_sensitivity = entity_data.mouse_sensitivity;
            // Gravity handled by RigidBody
            
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