#pragma once

#include "EngineExport.h"
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>
#include "Components/Components.hpp"
#include <entt/entt.hpp>

// Includes for parallel loading data structures
#include "Assets/CompiledMeshSerializer.hpp"
#include "Assets/CompiledTextureSerializer.hpp"
#include "Assets/AssetMetadata.hpp"
#include "Assets/LODGenerator.hpp"

// Forward declarations
class world;
class IRenderAPI;

// Entity types for serialization
enum class EntityType
{
    Static,      // No special components
    Renderable,  // Has mesh
    Collidable,  // Has collider only
    Physical,    // Has rigidbody and collider
    Player,      // Player entity
    Freecam,     // Freecam entity
    PlayerRep,   // Player representation
    PointLight,  // Point light source
    SpotLight    // Spot light source
};

// Level entity structure (runtime representation)
struct LevelEntity
{
    std::string name;
    EntityType type;

    // Transform data
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;

    // Component data
    std::string mesh_path;                  // Path to OBJ/glTF/GLB file
    std::vector<std::string> texture_paths; // Texture paths

    // Physics data
    bool has_rigidbody;
    float mass;
    bool apply_gravity;
    std::string body_motion_type = "Dynamic";

    bool has_collider;
    std::string collider_mesh_path;  // Separate collision mesh
    bool use_mesh_collision;         // Use visual mesh as collision if no collider specified

    // Collider shape data
    std::string collider_shape_type = "Mesh";
    glm::vec3 collider_box_half_extents = glm::vec3(0.5f);
    float collider_sphere_radius = 0.5f;
    float collider_capsule_half_height = 0.5f;
    float collider_capsule_radius = 0.3f;
    float collider_cylinder_half_height = 0.5f;
    float collider_cylinder_radius = 0.5f;
    float collider_friction = 0.2f;
    float collider_restitution = 0.0f;

    // Constraint data
    bool has_constraint = false;
    std::string constraint_type = "Fixed";
    std::string constraint_target_name;
    glm::vec3 constraint_anchor_1 = glm::vec3(0.0f);
    glm::vec3 constraint_anchor_2 = glm::vec3(0.0f);
    glm::vec3 constraint_hinge_axis = glm::vec3(0.0f, 1.0f, 0.0f);
    float constraint_hinge_min = -180.0f;
    float constraint_hinge_max = 180.0f;
    float constraint_min_distance = -1.0f;
    float constraint_max_distance = -1.0f;

    // Mesh rendering properties
    bool culling;
    bool transparent;
    bool visible;
    bool casts_shadow;
    int force_lod;

    // Player/Freecam specific
    float speed;              // For player/freecam
    float jump_force;         // For player
    float mouse_sensitivity;  // For player/freecam
    float movement_speed;     // For freecam normal speed
    float fast_movement_speed; // For freecam fast speed

    // Player representation specific
    std::string tracked_player_name;  // Name of player to track
    glm::vec3 position_offset;

    // Light properties (point and spot lights)
    glm::vec3 light_color{1.0f, 1.0f, 1.0f};
    float light_intensity = 1.0f;
    float light_range = 10.0f;
    float light_constant_attenuation = 1.0f;
    float light_linear_attenuation = 0.09f;
    float light_quadratic_attenuation = 0.032f;
    float light_inner_cone_angle = 12.5f;  // Spot light only
    float light_outer_cone_angle = 17.5f;  // Spot light only

    LevelEntity()
        : type(EntityType::Static)
        , position(0, 0, 0)
        , rotation(0, 0, 0)
        , scale(1, 1, 1)
        , has_rigidbody(false)
        , mass(1.0f)
        , apply_gravity(true)
        , has_collider(false)
        , use_mesh_collision(false)
        , culling(true)
        , transparent(false)
        , visible(true)
        , casts_shadow(true)
        , force_lod(-1)
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
    glm::vec3 gravity;
    float fixed_delta;

    // Lighting
    glm::vec3 ambient_light;
    glm::vec3 diffuse_light;
    glm::vec3 light_direction;

    LevelMetadata()
        : level_name("Untitled Level")
        , author("Unknown")
        , version("1.0")
        , entity_count(0)
        , gravity(0, -1, 0)
        , fixed_delta(1.0f / 60.0f)
        , ambient_light(0.2f, 0.2f, 0.2f)
        , diffuse_light(0.8f, 0.8f, 0.8f)
        , light_direction(0.0f, -1.0f, 0.0f)
    {}
};

// Level data structure
struct LevelData
{
    LevelMetadata metadata;
    std::vector<LevelEntity> entities;
};

// CPU-side pre-loaded mesh data (no GPU resources) for parallel loading
struct MeshPreloadData {
    enum class Type { None, GLTF, OBJ, Compiled };
    Type type = Type::None;
    std::string resolved_path;

    // GLTF: geometry-only result (from loadGltfGeometry, no render_api needed)
    std::unique_ptr<GltfLoadResult> gltf_geometry;

    // OBJ: parsed vertex data (CPU only)
    std::unique_ptr<ObjLoadResult> obj_result;

    // Compiled mesh: deserialized binary data (CPU only)
    std::unique_ptr<Assets::CompiledMeshData> compiled_data;

    // LOD data pre-loaded from disk
    bool has_lod_metadata = false;
    std::unique_ptr<Assets::AssetMetadata> lod_metadata;
    std::vector<Assets::LODMeshData> lod_mesh_data;

    // Pre-loaded compiled texture data (.ctex files, for compiled meshes)
    // Key: absolute texture file path
    struct PreloadedTexture {
        bool is_compiled = false;
        Assets::CompiledTextureData compiled_tex;
        bool success = false;
    };
    std::unordered_map<std::string, PreloadedTexture> preloaded_textures;

    bool success = false;
    std::string error_message;

    MeshPreloadData() = default;
    MeshPreloadData(MeshPreloadData&&) = default;
    MeshPreloadData& operator=(MeshPreloadData&&) = default;
    MeshPreloadData(const MeshPreloadData&) = delete;
    MeshPreloadData& operator=(const MeshPreloadData&) = delete;
};

class ENGINE_API LevelManager
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

    // Parallel instantiation - reads all meshes from disk in parallel, then
    // finalizes GPU uploads and entity creation on the main thread.
    bool instantiateLevelParallel(const LevelData& level_data,
                                  world& game_world,
                                  IRenderAPI* render_api,
                                  entt::entity* out_player_entity = nullptr,
                                  entt::entity* out_freecam_entity = nullptr,
                                  entt::entity* out_player_rep_entity = nullptr);

    // Cleanup - called before loading new level
    void cleanup();

    // Helper to load mesh (returns shared_ptr) - public so editor can use it
    std::shared_ptr<mesh> loadMesh(const LevelEntity& entity, IRenderAPI* render_api);
    std::shared_ptr<mesh> loadCompiledMesh(const std::string& cmesh_path, IRenderAPI* render_api);

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

    // Parallel loading helpers (called from instantiateLevelParallel)
    void preloadMeshCPU(MeshPreloadData& data);
    std::shared_ptr<mesh> finalizeMeshGPU(MeshPreloadData& preload,
                                           const LevelEntity& entity,
                                           IRenderAPI* render_api);
    std::shared_ptr<mesh> finalizeCompiledMeshGPU(MeshPreloadData& preload,
                                                   const LevelEntity& entity,
                                                   IRenderAPI* render_api);

    // Store level data to keep entity references valid
    std::vector<LevelEntity> stored_entities;

    // Binary format version (set during readBinaryHeader for readBinaryEntity)
    uint32_t binary_read_version = 0;
};