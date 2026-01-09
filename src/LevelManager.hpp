#pragma once

#include <string>
#include <vector>
#include <fstream>
#include "irrlicht/vector3.h"
#include "Components/gameObject.hpp"

// Forward declarations
class mesh;
class rigidbody;
class collider;
class playerEntity;
class freecamEntity;
class PlayerRepresentation;
class world;
class IRenderAPI;

using namespace irr;
using namespace core;

// Entity types for serialization
enum class EntityType
{
    Static,      // No special components
    Renderable,  // Has mesh
    Collidable,  // Has collider only
    Physical,    // Has rigidbody and collider
    Player,      // Player entity
    Freecam,     // Freecam entity
    PlayerRep    // Player representation
};

// Level entity structure (runtime representation)
struct LevelEntity
{
    std::string name;
    EntityType type;

    // Transform data
    vector3f position;
    vector3f rotation;
    vector3f scale;

    // Component data
    std::string mesh_path;                  // Path to OBJ/glTF/GLB file
    std::vector<std::string> texture_paths; // Texture paths

    // Physics data
    bool has_rigidbody;
    float mass;
    bool apply_gravity;

    bool has_collider;
    std::string collider_mesh_path;  // Separate collision mesh

    // Mesh rendering properties
    bool culling;
    bool transparent;
    bool visible;

    // Player/Freecam specific
    float speed;              // For player/freecam
    float jump_force;         // For player
    float mouse_sensitivity;  // For player/freecam
    float movement_speed;     // For freecam normal speed
    float fast_movement_speed; // For freecam fast speed

    // Player representation specific
    std::string tracked_player_name;  // Name of player to track
    vector3f position_offset;

    // Runtime pointers (populated during load)
    gameObject* game_object;
    mesh* mesh_component;
    rigidbody* rigidbody_component;
    collider* collider_component;

    LevelEntity()
        : type(EntityType::Static)
        , position(0, 0, 0)
        , rotation(0, 0, 0)
        , scale(1, 1, 1)
        , has_rigidbody(false)
        , mass(1.0f)
        , apply_gravity(true)
        , has_collider(false)
        , culling(true)
        , transparent(false)
        , visible(true)
        , speed(1.5f)
        , jump_force(3.0f)
        , mouse_sensitivity(1.0f)
        , movement_speed(5.0f)
        , fast_movement_speed(15.0f)
        , position_offset(0, 0, 0)
        , game_object(nullptr)
        , mesh_component(nullptr)
        , rigidbody_component(nullptr)
        , collider_component(nullptr)
    {}
};

// Level metadata
struct LevelMetadata
{
    std::string level_name;
    std::string author;
    std::string version;
    int entity_count;

    // World settings
    vector3f gravity;
    float fixed_delta;

    // Lighting
    vector3f ambient_light;
    vector3f diffuse_light;
    vector3f light_position;

    LevelMetadata()
        : level_name("Untitled Level")
        , author("Unknown")
        , version("1.0")
        , entity_count(0)
        , gravity(0, -1, 0)
        , fixed_delta(0.16f)
        , ambient_light(0.2f, 0.2f, 0.2f)
        , diffuse_light(0.8f, 0.8f, 0.8f)
        , light_position(1.0f, 1.0f, 1.0f)
    {}
};

// Level data structure
struct LevelData
{
    LevelMetadata metadata;
    std::vector<LevelEntity> entities;
};

class LevelManager
{
public:
    LevelManager();
    ~LevelManager();

    // Loading
    bool loadLevelFromJSON(const std::string& json_path, LevelData& out_level_data);
    bool loadLevelFromBinary(const std::string& binary_path, LevelData& out_level_data);
    bool loadLevel(const std::string& path, LevelData& out_level_data);  // Auto-detect format

    // Saving
    bool saveLevelToJSON(const std::string& json_path, const LevelData& level_data);
    bool saveLevelToBinary(const std::string& binary_path, const LevelData& level_data);

    // Compilation
    bool compileLevel(const std::string& json_path, const std::string& binary_path);

    // Instantiation - creates runtime objects from level data
    // Returns pointers to player and freecam entity data (for creating entities in main.cpp)
    bool instantiateLevel(const LevelData& level_data,
                         world& game_world,
                         IRenderAPI* render_api,
                         std::vector<mesh*>& out_meshes,
                         std::vector<rigidbody*>& out_rigidbodies,
                         std::vector<collider*>& out_colliders,
                         LevelEntity** out_player_data,
                         LevelEntity** out_freecam_data,
                         LevelEntity** out_player_rep_data);

    // Cleanup - called before loading new level
    void cleanup();

private:
    // JSON parsing
    bool parseMetadataFromJSON(const void* json, LevelMetadata& metadata);
    bool parseEntityFromJSON(const void* json, LevelEntity& entity);

    // Binary format helpers
    void writeBinaryHeader(std::ofstream& file, const LevelMetadata& metadata);
    void writeBinaryEntity(std::ofstream& file, const LevelEntity& entity);
    bool readBinaryHeader(std::ifstream& file, LevelMetadata& metadata);
    bool readBinaryEntity(std::ifstream& file, LevelEntity& entity);

    // String helpers for binary format
    void writeString(std::ofstream& file, const std::string& str);
    bool readString(std::ifstream& file, std::string& str);

    // Entity instantiation helpers
    gameObject* createGameObject(const LevelEntity& entity);
    mesh* createMesh(const LevelEntity& entity, gameObject& obj, IRenderAPI* render_api);
    rigidbody* createRigidbody(const LevelEntity& entity, gameObject& obj);
    collider* createCollider(const LevelEntity& entity, mesh* collider_mesh, gameObject& obj);

    // Resource management
    std::vector<gameObject*> owned_game_objects;
    std::vector<mesh*> owned_meshes;
    std::vector<rigidbody*> owned_rigidbodies;
    std::vector<collider*> owned_colliders;

    // Store level data to keep entity references valid
    std::vector<LevelEntity> stored_entities;
};
