#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include "irrlicht/vector3.h"
#include "Components/Components.hpp" 
#include <entt/entt.hpp>

// Forward declarations
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
    bool instantiateLevel(const LevelData& level_data,
                         world& game_world,
                         IRenderAPI* render_api,
                         entt::entity* out_player_entity = nullptr,
                         entt::entity* out_freecam_entity = nullptr,
                         entt::entity* out_player_rep_entity = nullptr);

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

    // Helper to load mesh (returns shared_ptr)
    std::shared_ptr<mesh> loadMesh(const LevelEntity& entity, IRenderAPI* render_api);

    // Store level data to keep entity references valid
    std::vector<LevelEntity> stored_entities;
};