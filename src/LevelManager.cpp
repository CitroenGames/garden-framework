#include "LevelManager.hpp"
#include "Thirdparty/tinygltf-2.9.6/json.hpp"
#include "Components/mesh.hpp"
#include "Components/rigidbody.hpp"
#include "Components/collider.hpp"
#include "Components/playerEntity.hpp"
#include "Components/FreecamEntity.hpp"
#include "Components/PlayerRepresentation.hpp"
#include "Components/camera.hpp"
#include "world.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/GltfLoader.hpp"
#include <iostream>
#include <cstring>

// Forward declaration (defined in main.cpp)
mesh* loadGltfMeshWithMaterials(const std::string& filename, gameObject& obj, IRenderAPI* render_api);

using json = nlohmann::json;

LevelManager::LevelManager()
{
}

LevelManager::~LevelManager()
{
    cleanup();
}

void LevelManager::cleanup()
{
    // Clean up owned resources
    for (auto* obj : owned_game_objects) delete obj;
    for (auto* m : owned_meshes) delete m;
    for (auto* rb : owned_rigidbodies) delete rb;
    for (auto* col : owned_colliders) delete col;

    owned_game_objects.clear();
    owned_meshes.clear();
    owned_rigidbodies.clear();
    owned_colliders.clear();
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
        if (lighting.contains("position"))
        {
            const auto& p = lighting["position"];
            metadata.light_position = vector3f(
                p.value("x", 1.0f),
                p.value("y", 1.0f),
                p.value("z", 1.0f)
            );
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
    // TODO: Implement JSON saving for level editor
    printf("JSON saving not yet implemented\n");
    return false;
}

bool LevelManager::loadLevelFromBinary(const std::string& binary_path, LevelData& out_level_data)
{
    // TODO: Implement binary loading
    printf("Binary loading not yet implemented\n");
    return false;
}

bool LevelManager::saveLevelToBinary(const std::string& binary_path, const LevelData& level_data)
{
    // TODO: Implement binary saving
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

gameObject* LevelManager::createGameObject(const LevelEntity& entity)
{
    gameObject* obj = new gameObject(entity.position.X, entity.position.Y, entity.position.Z);
    obj->rotation = entity.rotation;
    obj->scale = entity.scale;

    owned_game_objects.push_back(obj);
    return obj;
}

mesh* LevelManager::createMesh(const LevelEntity& entity, gameObject& obj, IRenderAPI* render_api)
{
    if (entity.mesh_path.empty())
    {
        return nullptr;
    }

    mesh* m = nullptr;

    // Check if glTF file (which might have materials)
    size_t path_len = entity.mesh_path.size();
    bool is_gltf = (path_len >= 5 && entity.mesh_path.substr(path_len - 5) == ".gltf") ||
                   (path_len >= 4 && entity.mesh_path.substr(path_len - 4) == ".glb");

    if (is_gltf)
    {
        // Use glTF loader with materials
        m = loadGltfMeshWithMaterials(entity.mesh_path, obj, render_api);
    }
    else
    {
        // Regular mesh loading (OBJ)
        m = new mesh(entity.mesh_path, obj);
    }

    if (m)
    {
        // Set mesh properties
        m->culling = entity.culling;
        m->transparent = entity.transparent;
        m->visible = entity.visible;

        // Load textures
        if (!entity.texture_paths.empty() && render_api)
        {
            for (const auto& tex_path : entity.texture_paths)
            {
                TextureHandle tex = render_api->loadTexture(tex_path, true, true);
                m->set_texture(tex);
                break; // For now, only use first texture
            }
        }

        owned_meshes.push_back(m);
    }

    return m;
}

rigidbody* LevelManager::createRigidbody(const LevelEntity& entity, gameObject& obj)
{
    if (!entity.has_rigidbody)
    {
        return nullptr;
    }

    rigidbody* rb = new rigidbody(obj);
    rb->mass = entity.mass;
    rb->apply_gravity = entity.apply_gravity;

    owned_rigidbodies.push_back(rb);
    return rb;
}

collider* LevelManager::createCollider(const LevelEntity& entity, mesh* collider_mesh, gameObject& obj)
{
    if (!entity.has_collider)
    {
        return nullptr;
    }

    // If no collider mesh provided, we need to load it
    mesh* col_mesh = collider_mesh;
    if (!col_mesh && !entity.collider_mesh_path.empty())
    {
        col_mesh = new mesh(entity.collider_mesh_path, obj);
        owned_meshes.push_back(col_mesh);
    }

    if (!col_mesh)
    {
        printf("WARNING: Entity '%s' has collider but no mesh specified\n", entity.name.c_str());
        return nullptr;
    }

    collider* col = new collider(*col_mesh, obj);
    owned_colliders.push_back(col);
    return col;
}

bool LevelManager::instantiateLevel(
    const LevelData& level_data,
    world& game_world,
    IRenderAPI* render_api,
    std::vector<mesh*>& out_meshes,
    std::vector<rigidbody*>& out_rigidbodies,
    std::vector<collider*>& out_colliders,
    LevelEntity** out_player_data,
    LevelEntity** out_freecam_data,
    LevelEntity** out_player_rep_data)
{
    printf("Instantiating level: %s\n", level_data.metadata.level_name.c_str());

    // Apply world settings
    game_world.setGravity(level_data.metadata.gravity);
    game_world.setFixedDelta(level_data.metadata.fixed_delta);

    // Store entities so pointers remain valid
    stored_entities = level_data.entities;

    // Initialize output pointers
    if (out_player_data) *out_player_data = nullptr;
    if (out_freecam_data) *out_freecam_data = nullptr;
    if (out_player_rep_data) *out_player_rep_data = nullptr;

    // Create all entities
    for (auto& entity : stored_entities)
    {
        gameObject* obj = createGameObject(entity);
        entity.game_object = obj;

        // Create mesh if needed
        if (entity.type == EntityType::Renderable ||
            entity.type == EntityType::Physical ||
            entity.type == EntityType::PlayerRep)
        {
            mesh* m = createMesh(entity, *obj, render_api);
            entity.mesh_component = m;
            if (m)
            {
                out_meshes.push_back(m);
            }
        }

        // Create rigidbody for physical entities
        if (entity.type == EntityType::Physical)
        {
            rigidbody* rb = createRigidbody(entity, *obj);
            entity.rigidbody_component = rb;
            if (rb)
            {
                out_rigidbodies.push_back(rb);
            }

            // Create collider
            collider* col = createCollider(entity, entity.mesh_component, *obj);
            entity.collider_component = col;
            if (col)
            {
                out_colliders.push_back(col);
            }
        }

        // Create collider-only entities
        if (entity.type == EntityType::Collidable)
        {
            collider* col = createCollider(entity, nullptr, *obj);
            entity.collider_component = col;
            if (col)
            {
                out_colliders.push_back(col);
            }
        }

        // Handle special entity types - store pointers for main.cpp to use
        if (entity.type == EntityType::Player)
        {
            // Create player rigidbody
            rigidbody* player_rb = createRigidbody(entity, *obj);
            entity.rigidbody_component = player_rb;
            if (player_rb)
            {
                out_rigidbodies.push_back(player_rb);
            }

            if (out_player_data)
            {
                *out_player_data = &entity;
            }

            printf("NOTE: Player entity '%s' created at position (%.2f, %.2f, %.2f)\n",
                   entity.name.c_str(), obj->position.X, obj->position.Y, obj->position.Z);
        }

        if (entity.type == EntityType::Freecam)
        {
            if (out_freecam_data)
            {
                *out_freecam_data = &entity;
            }

            printf("NOTE: Freecam entity '%s' created at position (%.2f, %.2f, %.2f)\n",
                   entity.name.c_str(), obj->position.X, obj->position.Y, obj->position.Z);
        }

        if (entity.type == EntityType::PlayerRep)
        {
            if (out_player_rep_data)
            {
                *out_player_rep_data = &entity;
            }

            printf("NOTE: Player representation '%s' created at position (%.2f, %.2f, %.2f)\n",
                   entity.name.c_str(), obj->position.X, obj->position.Y, obj->position.Z);
        }
    }

    printf("Level instantiation complete: %d entities\n", (int)stored_entities.size());
    return true;
}
