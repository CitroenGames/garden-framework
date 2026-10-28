#include "LevelManager.hpp"
#include "json.hpp"
#include "Components/Components.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
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
#include "Assets/MeshChunker.hpp"
#include "Assets/TerrainBuilder.hpp"
#include "Assets/AssetManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/ReflectionSerializer.hpp"
#include "Console/ConVar.hpp"
#include "GameFramework/GameMode.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "Threading/JobSystem.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using json = nlohmann::json;

static constexpr uint32_t LEVEL_BINARY_MAGIC   = 0x47444E4C; // "GDNL"
static constexpr uint32_t LEVEL_BINARY_VERSION  = 7;

static std::string readOptionValue(const std::string& options, std::initializer_list<const char*> keys)
{
    size_t pos = 0;
    while (pos < options.size())
    {
        while (pos < options.size() &&
               (options[pos] == '?' || options[pos] == '&' || options[pos] == ' '))
        {
            ++pos;
        }

        const size_t end = options.find_first_of("& ", pos);
        const std::string token = options.substr(pos, end == std::string::npos ? end : end - pos);
        const size_t equals = token.find('=');
        if (equals != std::string::npos)
        {
            const std::string key = token.substr(0, equals);
            for (const char* candidate : keys)
            {
                if (key == candidate)
                    return token.substr(equals + 1);
            }
        }

        if (end == std::string::npos)
            break;
        pos = end + 1;
    }

    return {};
}

static std::string resolveClassName(const std::string& option_override,
                                    const std::string& level_override,
                                    const std::string& project_default,
                                    const char* engine_fallback)
{
    if (!option_override.empty())
        return option_override;
    if (!level_override.empty())
        return level_override;
    if (!project_default.empty())
        return project_default;
    return engine_fallback;
}

static std::unique_ptr<GameFramework::GameModeBase> createGameModeOrFallback(const std::string& class_name)
{
    using namespace GameFramework;

    auto game_mode = GameModeRegistry::get().createGameMode(class_name);
    if (!game_mode && class_name != "GameMode")
        game_mode = GameModeRegistry::get().createGameMode("GameMode");
    return game_mode;
}

static std::unique_ptr<GameFramework::GameStateBase> createGameStateOrFallback(const std::string& class_name)
{
    using namespace GameFramework;

    auto game_state = GameModeRegistry::get().createGameState(class_name);
    if (!game_state && class_name != "GameState")
        game_state = GameModeRegistry::get().createGameState("GameState");
    return game_state;
}

static std::string resolvedGameModeClass(const LevelMetadata& metadata,
                                         const std::string& project_default,
                                         const std::string& options)
{
    return resolveClassName(
        readOptionValue(options, {"game", "game_mode", "game_mode_class"}),
        metadata.game_mode_class,
        project_default,
        "GameMode");
}

static std::string resolvedGameStateClass(const LevelMetadata& metadata,
                                          const std::string& project_default,
                                          const std::string& options)
{
    return resolveClassName(
        readOptionValue(options, {"game_state", "game_state_class"}),
        metadata.game_state_class,
        project_default,
        "GameState");
}

static void applyGameModeRuntimeSettings(GameFramework::GameModeBase& game_mode,
                                         const LevelMetadata& metadata)
{
    game_mode.setStartPlayersAsSpectators(metadata.start_players_as_spectators);
    game_mode.setPauseable(metadata.pauseable);

    if (auto* match_mode = dynamic_cast<GameFramework::GameMode*>(&game_mode))
    {
        match_mode->setDelayedStart(metadata.delayed_start);
        match_mode->setMinRespawnDelay(metadata.min_respawn_delay);
    }
}

class ScopedLoadTimer {
public:
    explicit ScopedLoadTimer(const char* label)
        : m_label(label)
        , m_start(std::chrono::steady_clock::now())
    {
    }

    ~ScopedLoadTimer()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_start);
        LOG_ENGINE_INFO("{} took {} ms", m_label, elapsed.count());
    }

private:
    const char* m_label;
    std::chrono::steady_clock::time_point m_start;
};

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

static MaterialLoaderConfig makeLevelMaterialLoaderConfig(const std::string& mesh_path, bool verbose_logging)
{
    MaterialLoaderConfig material_config;
    material_config.verbose_logging = verbose_logging;
    material_config.load_all_textures = false;
    material_config.priority_texture_types = {
        TextureType::BASE_COLOR,
        TextureType::DIFFUSE,
        TextureType::NORMAL
    };
    material_config.generate_mipmaps = true;
    material_config.flip_textures_vertically = true;
    material_config.cache_textures = true;

    size_t last_sep = mesh_path.find_last_of("/\\");
    material_config.texture_base_path = (last_sep != std::string::npos)
        ? mesh_path.substr(0, last_sep + 1)
        : "";

    return material_config;
}

static Assets::MeshChunkConfig getStaticMeshChunkConfig()
{
    Assets::MeshChunkConfig cfg;

    if (auto* cvar = CVAR_PTR(r_staticmesh_chunking))
        cfg.enabled = cvar->getBool();

    if (auto* cvar = CVAR_PTR(r_staticmesh_chunk_tris))
        cfg.target_triangles = static_cast<size_t>(std::max(cvar->getInt(), 1));

    if (auto* cvar = CVAR_PTR(r_staticmesh_max_chunks)) {
        int max_chunks = cvar->getInt();
        cfg.max_chunks = max_chunks > 0 ? static_cast<size_t>(max_chunks) : 0;
    }

    return cfg;
}

static uint32_t getTextureStreamingFirstMip(const Assets::CtexHeader& header)
{
    if (header.mip_count <= 1)
        return 0;

    auto* streaming_cvar = CVAR_PTR(r_texture_streaming);
    if (!streaming_cvar || !streaming_cvar->getBool())
        return 0;

    const int max_dimension = CVAR_PTR(r_texture_streaming_max_dimension)
        ? CVAR_PTR(r_texture_streaming_max_dimension)->getInt()
        : 0;
    if (max_dimension <= 0)
        return 0;

    uint32_t first_mip = 0;
    uint32_t largest_dimension = std::max(header.width, header.height);
    while (first_mip + 1 < header.mip_count &&
           largest_dimension > static_cast<uint32_t>(max_dimension))
    {
        ++first_mip;
        largest_dimension = std::max(1u, largest_dimension / 2u);
    }

    int min_resident = CVAR_PTR(r_texture_streaming_min_resident_mips)
        ? CVAR_PTR(r_texture_streaming_min_resident_mips)->getInt()
        : 1;
    min_resident = std::max(min_resident, 1);
    if (header.mip_count - first_mip < static_cast<uint32_t>(min_resident))
    {
        first_mip = header.mip_count > static_cast<uint32_t>(min_resident)
            ? header.mip_count - static_cast<uint32_t>(min_resident)
            : 0;
    }

    return first_mip;
}

static bool loadCompiledTextureDataForStreaming(Assets::CompiledTextureData& out,
                                                const std::string& ctex_path)
{
    Assets::CtexHeader header{};
    if (!Assets::CompiledTextureSerializer::loadHeader(header, ctex_path))
        return false;

    const uint32_t first_mip = getTextureStreamingFirstMip(header);
    const uint32_t resident_mips = header.mip_count - first_mip;

    bool ok = first_mip == 0
        ? Assets::CompiledTextureSerializer::load(out, ctex_path)
        : Assets::CompiledTextureSerializer::loadMipRange(out, ctex_path, first_mip, resident_mips);

    if (ok && CVAR_PTR(r_texture_streaming_debug) && CVAR_PTR(r_texture_streaming_debug)->getBool())
    {
        LOG_ENGINE_INFO("[TextureStreaming] {}: resident mips {}..{} of {} ({}x{})",
                        ctex_path,
                        out.first_mip,
                        out.first_mip + static_cast<uint32_t>(out.mip_levels.size()) - 1,
                        out.header.mip_count,
                        out.header.width,
                        out.header.height);
    }

    return ok;
}

static void copyMaterialPayload(MaterialRange& dst, const MaterialRange& src)
{
    dst.texture = src.texture;
    dst.material_name = src.material_name;
    dst.alpha_mode = src.alpha_mode;
    dst.alpha_cutoff = src.alpha_cutoff;
    dst.double_sided = src.double_sided;
    dst.material_flags = src.material_flags;
    dst.metallic_factor = src.metallic_factor;
    dst.roughness_factor = src.roughness_factor;
    dst.emissive_factor = src.emissive_factor;
    dst.base_color_factor = src.base_color_factor;
    dst.metallic_roughness_texture = src.metallic_roughness_texture;
    dst.normal_texture = src.normal_texture;
    dst.occlusion_texture = src.occlusion_texture;
    dst.emissive_texture = src.emissive_texture;
}

using LevelMeshResolver = std::function<std::shared_ptr<mesh>(const std::string&, const LevelEntity&)>;

static bool entityTypeCanHaveLevelCollider(EntityType type)
{
    return type == EntityType::Renderable ||
           type == EntityType::Physical ||
           type == EntityType::Collidable ||
           type == EntityType::Player;
}

static void copyColliderSettings(ColliderComponent& col, const LevelEntity& entity_data)
{
    col.shape_type = stringToColliderShapeType(entity_data.collider_shape_type);
    col.box_half_extents = entity_data.collider_box_half_extents;
    col.sphere_radius = entity_data.collider_sphere_radius;
    col.capsule_half_height = entity_data.collider_capsule_half_height;
    col.capsule_radius = entity_data.collider_capsule_radius;
    col.cylinder_half_height = entity_data.collider_cylinder_half_height;
    col.cylinder_radius = entity_data.collider_cylinder_radius;
    col.friction = entity_data.collider_friction;
    col.restitution = entity_data.collider_restitution;
}

static bool readVec3Json(const json& value, glm::vec3& out)
{
    if (value.is_array() && value.size() >= 3 &&
        value[0].is_number() && value[1].is_number() && value[2].is_number())
    {
        out = glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
        return true;
    }

    if (value.is_object())
    {
        out = glm::vec3(
            value.value("x", value.value("r", out.x)),
            value.value("y", value.value("g", out.y)),
            value.value("z", value.value("b", out.z)));
        return true;
    }

    return false;
}

static std::string readEnumName(const json& object, const char* key, const std::string& fallback)
{
    if (!object.contains(key))
        return fallback;
    const auto& value = object[key];
    if (value.is_string())
        return value.get<std::string>();
    return fallback;
}

static bool hasComponentJson(const json& components, const char* name)
{
    return components.is_object() && components.contains(name) && components[name].is_object();
}

static void applyReflectedComponentsToLevelEntity(const json& components, LevelEntity& entity)
{
    if (!components.is_object())
        return;

    if (hasComponentJson(components, "TagComponent"))
    {
        const auto& c = components["TagComponent"];
        if (c.contains("name") && c["name"].is_string())
            entity.name = c["name"].get<std::string>();
    }

    if (hasComponentJson(components, "TransformComponent"))
    {
        const auto& c = components["TransformComponent"];
        if (c.contains("position")) readVec3Json(c["position"], entity.position);
        if (c.contains("rotation")) readVec3Json(c["rotation"], entity.rotation);
        if (c.contains("scale"))    readVec3Json(c["scale"], entity.scale);
    }

    if (hasComponentJson(components, "RigidBodyComponent"))
    {
        const auto& c = components["RigidBodyComponent"];
        entity.has_rigidbody = true;
        entity.mass = c.value("mass", entity.mass);
        entity.apply_gravity = c.value("apply_gravity", entity.apply_gravity);
        entity.body_motion_type = readEnumName(c, "motion_type", entity.body_motion_type);
    }

    if (hasComponentJson(components, "ColliderComponent"))
    {
        const auto& c = components["ColliderComponent"];
        entity.has_collider = true;
        entity.collider_shape_type = readEnumName(c, "shape_type", entity.collider_shape_type);
        if (c.contains("box_half_extents")) readVec3Json(c["box_half_extents"], entity.collider_box_half_extents);
        entity.collider_sphere_radius = c.value("sphere_radius", entity.collider_sphere_radius);
        entity.collider_capsule_half_height = c.value("capsule_half_height", entity.collider_capsule_half_height);
        entity.collider_capsule_radius = c.value("capsule_radius", entity.collider_capsule_radius);
        entity.collider_cylinder_half_height = c.value("cylinder_half_height", entity.collider_cylinder_half_height);
        entity.collider_cylinder_radius = c.value("cylinder_radius", entity.collider_cylinder_radius);
        entity.collider_friction = c.value("friction", entity.collider_friction);
        entity.collider_restitution = c.value("restitution", entity.collider_restitution);
    }

    if (hasComponentJson(components, "ConstraintComponent"))
    {
        const auto& c = components["ConstraintComponent"];
        entity.has_constraint = true;
        entity.constraint_type = readEnumName(c, "type", entity.constraint_type);
        if (c.contains("target_entity_name") && c["target_entity_name"].is_string())
            entity.constraint_target_name = c["target_entity_name"].get<std::string>();
        if (c.contains("anchor_1")) readVec3Json(c["anchor_1"], entity.constraint_anchor_1);
        if (c.contains("anchor_2")) readVec3Json(c["anchor_2"], entity.constraint_anchor_2);
        if (c.contains("hinge_axis")) readVec3Json(c["hinge_axis"], entity.constraint_hinge_axis);
        entity.constraint_hinge_min = c.value("hinge_min_limit", entity.constraint_hinge_min);
        entity.constraint_hinge_max = c.value("hinge_max_limit", entity.constraint_hinge_max);
        entity.constraint_min_distance = c.value("min_distance", entity.constraint_min_distance);
        entity.constraint_max_distance = c.value("max_distance", entity.constraint_max_distance);
    }

    if (hasComponentJson(components, "PlayerComponent"))
    {
        const auto& c = components["PlayerComponent"];
        entity.type = EntityType::Player;
        entity.speed = c.value("speed", entity.speed);
        entity.jump_force = c.value("jump_force", entity.jump_force);
        entity.mouse_sensitivity = c.value("mouse_sensitivity", entity.mouse_sensitivity);
        entity.has_rigidbody = true;
    }
    else if (hasComponentJson(components, "FreecamComponent"))
    {
        const auto& c = components["FreecamComponent"];
        entity.type = EntityType::Freecam;
        entity.movement_speed = c.value("movement_speed", entity.movement_speed);
        entity.fast_movement_speed = c.value("fast_movement_speed", entity.fast_movement_speed);
        entity.mouse_sensitivity = c.value("mouse_sensitivity", entity.mouse_sensitivity);
    }
    else if (hasComponentJson(components, "PlayerRepresentationComponent"))
    {
        const auto& c = components["PlayerRepresentationComponent"];
        entity.type = EntityType::PlayerRep;
        if (c.contains("position_offset")) readVec3Json(c["position_offset"], entity.position_offset);
    }
    else if (hasComponentJson(components, "PointLightComponent"))
    {
        const auto& c = components["PointLightComponent"];
        entity.type = EntityType::PointLight;
        if (c.contains("color")) readVec3Json(c["color"], entity.light_color);
        entity.light_intensity = c.value("intensity", entity.light_intensity);
        entity.light_range = c.value("range", entity.light_range);
        entity.light_constant_attenuation = c.value("constant_attenuation", entity.light_constant_attenuation);
        entity.light_linear_attenuation = c.value("linear_attenuation", entity.light_linear_attenuation);
        entity.light_quadratic_attenuation = c.value("quadratic_attenuation", entity.light_quadratic_attenuation);
    }
    else if (hasComponentJson(components, "SpotLightComponent"))
    {
        const auto& c = components["SpotLightComponent"];
        entity.type = EntityType::SpotLight;
        if (c.contains("color")) readVec3Json(c["color"], entity.light_color);
        entity.light_intensity = c.value("intensity", entity.light_intensity);
        entity.light_range = c.value("range", entity.light_range);
        entity.light_inner_cone_angle = c.value("inner_cone_angle", entity.light_inner_cone_angle);
        entity.light_outer_cone_angle = c.value("outer_cone_angle", entity.light_outer_cone_angle);
        entity.light_constant_attenuation = c.value("constant_attenuation", entity.light_constant_attenuation);
        entity.light_linear_attenuation = c.value("linear_attenuation", entity.light_linear_attenuation);
        entity.light_quadratic_attenuation = c.value("quadratic_attenuation", entity.light_quadratic_attenuation);
    }
    else if (entity.has_rigidbody && entity.has_collider)
    {
        entity.type = EntityType::Physical;
    }
    else if (entity.has_collider && entity.mesh_path.empty())
    {
        entity.type = EntityType::Collidable;
    }
    else if (!entity.mesh_path.empty())
    {
        entity.type = EntityType::Renderable;
    }
}

static void deserializeReflectedComponents(ReflectionRegistry* reflection,
                                           entt::registry& registry,
                                           entt::entity entity,
                                           const json& components)
{
    if (!reflection || !components.is_object() || components.empty())
        return;

    json entity_json = json::object();
    entity_json["components"] = components;
    ReflectionSerializer::deserializeEntity(registry, entity, entity_json, *reflection);
}

struct LevelInstantiationPlan
{
    std::vector<EntityGuid> identity_guids;
    std::vector<json> reflected_components;
    // Parent entity's index in the LevelData cohort, or size_t max for roots.
    std::vector<size_t> parent_indices;
};

static Hierarchy::PhysicsAttachmentNode stagedPhysicsAttachmentNode(
    const LevelEntity& entity)
{
    Hierarchy::PhysicsAttachmentNode node;
    node.local_scale = entity.scale;
    node.has_rigid_body = entity.has_rigidbody ||
                          entity.type == EntityType::Physical ||
                          entity.type == EntityType::Player;
    node.rigid_body_motion = stringToBodyMotionType(entity.body_motion_type);
    node.has_character_controller =
        entity.type == EntityType::Player ||
        hasComponentJson(entity.reflected_components,
                         "CharacterControllerComponent");

    const bool has_static_collider =
        !node.has_rigid_body &&
        ((entity.has_collider && entityTypeCanHaveLevelCollider(entity.type)) ||
         entity.use_mesh_collision);
    // setupLevelColliderComponent turns a visual-mesh collision request into
    // a Mesh ColliderComponent. Level physics then creates that collider as a
    // static mesh body even when the authored RigidBody is kinematic, matching
    // the direct-attach predicate for a final mesh ColliderComponent.
    const bool has_kinematic_mesh_body =
        node.has_rigid_body &&
        node.rigid_body_motion == BodyMotionType::Kinematic &&
        ((entity.has_collider && entity.collider_shape_type == "Mesh") ||
         entity.use_mesh_collision);
    bool has_terrain_collision = false;
    if (hasComponentJson(entity.reflected_components, "TerrainComponent"))
    {
        const json& terrain = entity.reflected_components["TerrainComponent"];
        has_terrain_collision = terrain.value("generate_collision", true);
    }
    node.has_non_kinematic_physics_body =
        has_static_collider || has_kinematic_mesh_body ||
        (!node.has_rigid_body &&
         hasComponentJson(entity.reflected_components, "TriggerComponent")) ||
        has_terrain_collision;
    return node;
}

static bool parseCanonicalGuid(const json& value, EntityGuid& out)
{
    if (!value.is_string())
        return false;
    const std::string text = value.get<std::string>();
    if (text.size() != 33 || text[16] != '-')
        return false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (i == 16)
            continue;
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return guidFromString(text, out);
}

static bool prepareLevelInstantiation(const LevelData& level_data,
                                      const entt::registry& registry,
                                      LevelInstantiationPlan& plan)
{
    const size_t entity_count = level_data.entities.size();
    const size_t no_parent = std::numeric_limits<size_t>::max();
    plan.identity_guids.resize(entity_count);
    plan.reflected_components.resize(entity_count);
    plan.parent_indices.assign(entity_count, no_parent);

    std::unordered_set<EntityGuid, GuidHash> destination_guids;
    for (auto [e, identity] : registry.view<IdentityComponent>().each())
    {
        (void)e;
        if (!identity.guid.isNull())
            destination_guids.insert(identity.guid);
    }

    std::vector<bool> has_serialized_identity(entity_count, false);
    std::unordered_set<EntityGuid, GuidHash> cohort_guids;
    cohort_guids.reserve(entity_count);

    // First reserve and validate all persisted values so generated legacy IDs
    // cannot collide with a later entity in this same file.
    for (size_t i = 0; i < entity_count; ++i)
    {
        const json& components = level_data.entities[i].reflected_components;
        if (!components.is_object())
            continue;
        auto identity_it = components.find("IdentityComponent");
        if (identity_it == components.end() || identity_it->is_null())
            continue;
        if (!identity_it->is_object())
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: IdentityComponent for '{}' is malformed",
                             level_data.entities[i].name);
            return false;
        }

        auto guid_it = identity_it->find("guid");
        if (guid_it == identity_it->end() || guid_it->is_null())
            continue;

        EntityGuid guid{};
        if (!parseCanonicalGuid(*guid_it, guid))
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: malformed IdentityComponent GUID for '{}'",
                             level_data.entities[i].name);
            return false;
        }
        if (guid.isNull())
            continue;
        if (destination_guids.find(guid) != destination_guids.end())
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: IdentityComponent GUID collision with destination registry for '{}'",
                             level_data.entities[i].name);
            return false;
        }
        if (!cohort_guids.insert(guid).second)
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: duplicate IdentityComponent GUID in level cohort for '{}'",
                             level_data.entities[i].name);
            return false;
        }
        plan.identity_guids[i] = guid;
        has_serialized_identity[i] = true;
    }

    for (size_t i = 0; i < entity_count; ++i)
    {
        if (has_serialized_identity[i])
            continue;
        EntityGuid guid{};
        do
        {
            guid = generateEntityGuid();
        } while (guid.isNull() ||
                 destination_guids.find(guid) != destination_guids.end() ||
                 !cohort_guids.insert(guid).second);
        plan.identity_guids[i] = guid;
    }

    std::unordered_map<EntityGuid, size_t, GuidHash> cohort_by_guid;
    cohort_by_guid.reserve(entity_count);
    for (size_t i = 0; i < entity_count; ++i)
        cohort_by_guid.emplace(plan.identity_guids[i], i);

    // Parent is validated separately and installed only after every entity
    // and final identity exists. ChildrenComponent is an ignored mirror.
    for (size_t i = 0; i < entity_count; ++i)
    {
        const LevelEntity& entity = level_data.entities[i];
        const json& components = entity.reflected_components;
        json normalized = components.is_object() ? components : json::object();
        // Identity was selected by preflight and installed by scoped restore.
        // Reflection must never replace that authoritative value.
        normalized.erase("IdentityComponent");
        normalized.erase("ParentComponent");
        normalized.erase("ChildrenComponent");
        plan.reflected_components[i] = std::move(normalized);

        if (!components.is_object())
            continue;
        auto parent_it = components.find("ParentComponent");
        if (parent_it == components.end() || parent_it->is_null())
            continue;
        if (!parent_it->is_object())
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: ParentComponent for '{}' is malformed",
                             entity.name);
            return false;
        }
        auto ref_it = parent_it->find("parent");
        if (ref_it == parent_it->end() || ref_it->is_null())
            continue;

        EntityGuid parent_guid{};
        if (!parseCanonicalGuid(*ref_it, parent_guid))
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: malformed ParentComponent GUID for '{}'",
                             entity.name);
            return false;
        }
        if (parent_guid.isNull())
            continue;

        const auto parent_entity = cohort_by_guid.find(parent_guid);
        if (parent_entity == cohort_by_guid.end())
        {
            LOG_ENGINE_WARN("Level entity '{}' references a missing or external parent {}; parent link removed",
                            entity.name, guidToString(parent_guid));
            continue;
        }
        if (parent_entity->second == i)
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: '{}' is its own hierarchy parent",
                             entity.name);
            return false;
        }
        plan.parent_indices[i] = parent_entity->second;
    }

    // Each entity has at most one parent. A three-state walk rejects cycles
    // without modifying the destination registry.
    std::vector<uint8_t> visit_state(entity_count, 0);
    for (size_t start = 0; start < entity_count; ++start)
    {
        if (visit_state[start] != 0)
            continue;
        std::vector<size_t> path;
        size_t current = start;
        while (current != no_parent && visit_state[current] == 0)
        {
            visit_state[current] = 1;
            path.push_back(current);
            current = plan.parent_indices[current];
        }
        if (current != no_parent && visit_state[current] == 1)
        {
            LOG_ENGINE_ERROR("Level instantiation rejected: hierarchy parent links contain a cycle");
            return false;
        }
        for (size_t index : path)
            visit_state[index] = 2;
    }

    // Apply the direct-attach policy while the level is still staged. A
    // rejection occurs before registry mutation or Jolt body creation.
    std::vector<Hierarchy::PhysicsAttachmentNode> physics_nodes;
    physics_nodes.reserve(entity_count);
    for (const LevelEntity& entity : level_data.entities)
        physics_nodes.push_back(stagedPhysicsAttachmentNode(entity));
    for (size_t child_index = 0; child_index < entity_count; ++child_index)
    {
        const size_t direct_parent = plan.parent_indices[child_index];
        if (direct_parent == no_parent)
            continue;

        std::vector<Hierarchy::PhysicsAttachmentNode> ancestors;
        for (size_t current = direct_parent; current != no_parent;
             current = plan.parent_indices[current])
        {
            ancestors.push_back(physics_nodes[current]);
        }
        const auto result = Hierarchy::validatePhysicsAttachment(
            physics_nodes[child_index], ancestors);
        if (result != Hierarchy::PhysicsAttachmentValidationResult::Allowed)
        {
            LOG_ENGINE_ERROR(
                "Level instantiation rejected: hierarchy physics policy ({}) for '{}'",
                static_cast<int>(result), level_data.entities[child_index].name);
            return false;
        }
    }

    return true;
}

static void applyWaterComponentToEntityMesh(entt::registry& registry, entt::entity entity)
{
    auto* water = registry.try_get<WaterComponent>(entity);
    auto* mesh_component = registry.try_get<MeshComponent>(entity);
    if (water && mesh_component && mesh_component->m_mesh)
        applyWaterComponentToMesh(*water, *mesh_component->m_mesh);
}

static std::shared_ptr<mesh> getVisualMesh(entt::registry& registry, entt::entity e)
{
    if (!registry.all_of<MeshComponent>(e))
        return nullptr;
    return registry.get<MeshComponent>(e).m_mesh;
}

static void applyMeshCollisionMetadata(const std::shared_ptr<mesh>& m_ptr,
                                       const std::string& resolved_source_path)
{
    if (!m_ptr || resolved_source_path.empty())
        return;

    m_ptr->source_path = resolved_source_path;
    m_ptr->collision_cache_path.clear();
    m_ptr->collision_source_hash = 0;
    m_ptr->collision_source_file_size = 0;

    Assets::AssetMetadata metadata;
    const std::string meta_path = Assets::AssetMetadataSerializer::getMetaPath(resolved_source_path);
    if (!Assets::AssetMetadataSerializer::load(metadata, meta_path))
        return;

    if (!metadata.collision.enabled || metadata.collision.file_path.empty())
        return;

    std::filesystem::path collision_path(metadata.collision.file_path);
    if (collision_path.is_relative())
        collision_path = std::filesystem::path(resolved_source_path).parent_path() / collision_path;

    m_ptr->collision_cache_path = collision_path.string();
    m_ptr->collision_source_hash = metadata.collision.source_hash != 0
        ? metadata.collision.source_hash
        : metadata.source_hash;
    m_ptr->collision_source_file_size = metadata.collision.source_file_size != 0
        ? metadata.collision.source_file_size
        : metadata.source_file_size;
}

static void assignColliderMesh(ColliderComponent& col,
                               const LevelEntity& entity_data,
                               entt::registry& registry,
                               entt::entity e,
                               const LevelMeshResolver& resolve_mesh)
{
    if (!entity_data.collider_mesh_path.empty())
    {
        LevelEntity col_ent = entity_data;
        col_ent.mesh_path = entity_data.collider_mesh_path;
        col_ent.texture_paths.clear();
        col.m_mesh = resolve_mesh(entity_data.collider_mesh_path, col_ent);

        if (!col.m_mesh && entity_data.use_mesh_collision)
        {
            col.m_mesh = getVisualMesh(registry, e);
            if (col.m_mesh)
            {
                LOG_ENGINE_WARN("Entity '{}': collider mesh '{}' failed to load, using visual mesh because use_mesh_collision=true",
                                entity_data.name, entity_data.collider_mesh_path);
            }
        }

        if (!col.m_mesh)
        {
            LOG_ENGINE_ERROR("Entity '{}': collider mesh '{}' failed to load",
                             entity_data.name, entity_data.collider_mesh_path);
        }
        return;
    }

    col.m_mesh = getVisualMesh(registry, e);
}

static void setupLevelColliderComponent(entt::registry& registry,
                                        entt::entity e,
                                        const LevelEntity& entity_data,
                                        const LevelMeshResolver& resolve_mesh)
{
    if (entity_data.has_collider && entityTypeCanHaveLevelCollider(entity_data.type))
    {
        auto& col = registry.get_or_emplace<ColliderComponent>(e);
        copyColliderSettings(col, entity_data);
        assignColliderMesh(col, entity_data, registry, e, resolve_mesh);
    }

    if (entity_data.use_mesh_collision && registry.all_of<MeshComponent>(e))
    {
        auto& col = registry.get_or_emplace<ColliderComponent>(e);
        if (!entity_data.has_collider)
        {
            col.shape_type = ColliderShapeType::Mesh;
        }
        if (!col.m_mesh)
        {
            col.m_mesh = getVisualMesh(registry, e);
            LOG_ENGINE_INFO("Entity '{}': using visual mesh as collision (use_mesh_collision=true)", entity_data.name);
        }
    }
}

static bool setupLevelTerrainComponent(entt::registry& registry,
                                       entt::entity e,
                                       const LevelEntity& entity_data,
                                       IRenderAPI* render_api)
{
    auto* terrain = registry.try_get<TerrainComponent>(e);
    if (!terrain || terrain->heightmap_path.empty())
        return true;

    const std::string resolved_heightmap =
        Assets::AssetManager::get().resolveAssetPath(terrain->heightmap_path);
    const bool use_gpu_displacement =
        terrain->gpu_displacement && render_api && render_api->supportsHeightmapDisplacement();

    Assets::TerrainBuildResult terrain_mesh =
        Assets::TerrainBuilder::buildFromHeightmap(*terrain, resolved_heightmap, use_gpu_displacement);
    if (!terrain_mesh.success || !terrain_mesh.render_mesh)
    {
        LOG_ENGINE_ERROR("Entity '{}': failed to build terrain: {}",
                         entity_data.name, terrain_mesh.error_message);
        return false;
    }

    auto render_mesh = terrain_mesh.render_mesh;
    if (!terrain->albedo_path.empty() && render_api)
    {
        const std::string resolved_albedo =
            Assets::AssetManager::get().resolveAssetPath(terrain->albedo_path);
        const TextureHandle albedo = render_api->loadTexture(resolved_albedo, false, true);
        render_mesh->set_texture(albedo);
    }

    if (use_gpu_displacement && render_api)
    {
        const TextureHandle height_texture = render_api->loadTexture(resolved_heightmap, false, false);
        render_mesh->heightmap_displacement = height_texture != INVALID_TEXTURE;
        render_mesh->heightmap_texture = height_texture;
        render_mesh->heightmap_height_scale = terrain->height_scale;
        render_mesh->heightmap_height_offset = terrain->height_offset;
        render_mesh->heightmap_texel_size = glm::vec2(
            terrain_mesh.heightmap_width > 0 ? 1.0f / static_cast<float>(terrain_mesh.heightmap_width) : 0.0f,
            terrain_mesh.heightmap_height > 0 ? 1.0f / static_cast<float>(terrain_mesh.heightmap_height) : 0.0f);
    }

    if (render_api && render_mesh->is_valid && !render_mesh->isUploadedToGPU())
        render_mesh->uploadToGPU(render_api);

    auto& mesh_component = registry.get_or_emplace<MeshComponent>(e);
    mesh_component.m_mesh = render_mesh;

    if (terrain->generate_collision && terrain_mesh.collision_mesh)
    {
        auto& col = registry.get_or_emplace<ColliderComponent>(e);
        col.shape_type = ColliderShapeType::Mesh;
        col.m_mesh = terrain_mesh.collision_mesh;
        col.friction = terrain->friction;
        col.restitution = terrain->restitution;
        LOG_ENGINE_INFO("Entity '{}': using heightmap terrain collision", entity_data.name);
    }
    return true;
}

static PhysicsSystem::PhysicsBodyDesc makeBodyDesc(const LevelEntity& entity_data, const ColliderComponent& col, bool player_body = false)
{
    PhysicsSystem::PhysicsBodyDesc desc;
    desc.mass = entity_data.mass;
    desc.friction = col.friction;
    desc.restitution = col.restitution;
    desc.apply_gravity = player_body ? false : entity_data.apply_gravity;
    desc.lock_rotation = true;
    return desc;
}

// The hierarchy policy has already established unit scale on every ancestor
// of a kinematic child. Resolve its world pose directly from authoritative
// local ECS data, rather than waiting for the derived transform cache.
static bool resolveLevelKinematicBodyPose(entt::registry& registry,
                                          entt::entity entity,
                                          glm::vec3& out_position,
                                          glm::vec3& out_rotation)
{
    const glm::mat4 world = Hierarchy::computeWorldMatrix(registry, entity);
    glm::vec3 scale{}, translation{}, skew{};
    glm::quat orientation{};
    glm::vec4 perspective{};
    if (!glm::decompose(world, scale, orientation, translation, skew, perspective))
        return false;

    const float orientation_length_sq = glm::dot(orientation, orientation);
    if (!std::isfinite(translation.x) || !std::isfinite(translation.y) ||
        !std::isfinite(translation.z) || !std::isfinite(orientation.x) ||
        !std::isfinite(orientation.y) || !std::isfinite(orientation.z) ||
        !std::isfinite(orientation.w) || !std::isfinite(orientation_length_sq) ||
        orientation_length_sq <= 1e-12f)
    {
        return false;
    }

    orientation = glm::normalize(orientation);
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(orientation), yaw, pitch, roll);
    out_position = translation;
    out_rotation = glm::vec3(glm::degrees(pitch), glm::degrees(yaw),
                             glm::degrees(roll));
    return true;
}

static bool createLevelPhysicsBody(world& game_world, entt::entity e, const LevelEntity& entity_data)
{
    if (!game_world.registry.all_of<ColliderComponent, TransformComponent>(e))
        return false;
    if (entity_data.type == EntityType::Player)
        return false;

    auto& col = game_world.registry.get<ColliderComponent>(e);
    auto& t = game_world.registry.get<TransformComponent>(e);
    PhysicsSystem::PhysicsBodyDesc desc = makeBodyDesc(entity_data, col);

    if (entity_data.type == EntityType::Physical)
    {
        BodyMotionType motion = BodyMotionType::Dynamic;
        if (game_world.registry.all_of<RigidBodyComponent>(e))
            motion = game_world.registry.get<RigidBodyComponent>(e).motion_type;

        if (motion == BodyMotionType::Kinematic)
        {
            glm::vec3 body_position = t.position;
            glm::vec3 body_rotation = t.rotation;
            if (Hierarchy::parentOf(game_world.registry, e) != entt::null &&
                !resolveLevelKinematicBodyPose(game_world.registry, e,
                                               body_position, body_rotation))
            {
                LOG_ENGINE_ERROR(
                    "Entity '{}': failed to resolve composed hierarchy pose for kinematic body initialization",
                    entity_data.name);
                return false;
            }
            if (col.shape_type == ColliderShapeType::Mesh)
            {
                if (!col.is_mesh_valid())
                {
                    LOG_ENGINE_ERROR("Entity '{}': kinematic mesh collider has no valid mesh", entity_data.name);
                    return false;
                }
                return game_world.getPhysicsSystem().createStaticMeshBody(body_position, body_rotation, t.scale, *col.get_mesh(), e, desc).IsInvalid() == false;
            }

            JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
            if (!shape)
            {
                LOG_ENGINE_ERROR("Entity '{}': failed to create kinematic collider shape '{}'",
                                 entity_data.name, colliderShapeTypeToString(col.shape_type));
                return false;
            }
            return game_world.getPhysicsSystem().createKinematicBody(body_position, body_rotation, shape, e, desc).IsInvalid() == false;
        }

        if (col.shape_type == ColliderShapeType::Mesh)
        {
            if (!col.is_mesh_valid())
            {
                LOG_ENGINE_ERROR("Entity '{}': dynamic mesh collider has no valid mesh", entity_data.name);
                return false;
            }
            col.shape_type = ColliderShapeType::ConvexHull;
            LOG_ENGINE_WARN("Entity '{}': Mesh collider on dynamic body not supported, using ConvexHull", entity_data.name);
        }

        JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
        if (!shape)
        {
            LOG_ENGINE_ERROR("Entity '{}': failed to create dynamic collider shape '{}'",
                             entity_data.name, colliderShapeTypeToString(col.shape_type));
            return false;
        }
        return game_world.getPhysicsSystem().createDynamicBody(t.position, t.rotation, shape, e, desc).IsInvalid() == false;
    }

    if (col.shape_type == ColliderShapeType::Mesh)
    {
        if (!col.is_mesh_valid())
        {
            LOG_ENGINE_ERROR("Entity '{}': static mesh collider has no valid mesh", entity_data.name);
            return false;
        }
        return game_world.getPhysicsSystem().createStaticMeshBody(t.position, t.rotation, t.scale, *col.get_mesh(), e, desc).IsInvalid() == false;
    }

    JPH::ShapeRefC shape = PhysicsSystem::createShapeFromCollider(col, t.scale);
    if (!shape)
    {
        LOG_ENGINE_ERROR("Entity '{}': failed to create static collider shape '{}'",
                         entity_data.name, colliderShapeTypeToString(col.shape_type));
        return false;
    }
    return game_world.getPhysicsSystem().createStaticBody(t.position, t.rotation, shape, e, desc).IsInvalid() == false;
}

static const MaterialRange* findMaterialTemplate(const std::vector<MaterialRange>& ranges, size_t source_range)
{
    for (const auto& range : ranges)
        if (range.source_range == source_range)
            return &range;

    if (source_range < ranges.size())
        return &ranges[source_range];

    return nullptr;
}

static std::vector<bool> buildSplitMaskFromRanges(const std::vector<MaterialRange>& ranges)
{
    std::vector<bool> split_mask;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        size_t source_id = range.source_range == std::numeric_limits<size_t>::max()
            ? i
            : range.source_range;
        if (source_id >= split_mask.size())
            split_mask.resize(source_id + 1, true);
        if (range.isAlphaBlend())
            split_mask[source_id] = false;
    }
    return split_mask;
}

static bool hasAlphaBlendRange(const std::vector<MaterialRange>& ranges)
{
    return std::any_of(ranges.begin(), ranges.end(),
                       [](const MaterialRange& range) { return range.isAlphaBlend(); });
}

static Assets::LODMeshData maybeChunkLODForRuntime(
    const Assets::LODMeshData& lod_data,
    const std::vector<MaterialRange>& material_templates,
    bool mesh_transparent)
{
    Assets::MeshChunkConfig cfg = getStaticMeshChunkConfig();
    if (!cfg.enabled || lod_data.vertices.empty() || lod_data.indices.empty())
        return lod_data;

    std::vector<bool> split_mask = buildSplitMaskFromRanges(material_templates);
    if (mesh_transparent && !hasAlphaBlendRange(material_templates)) {
        return lod_data;
    }

    const std::vector<bool>* split_ptr = split_mask.empty() ? nullptr : &split_mask;
    return Assets::MeshChunker::chunkLODMesh(lod_data, cfg, split_ptr);
}

static bool uploadChunkedMaterialMesh(const std::shared_ptr<mesh>& m_ptr, IRenderAPI* render_api)
{
    if (!m_ptr || !render_api || !m_ptr->is_valid || !m_ptr->vertices || m_ptr->vertices_len < 3)
        return false;

    Assets::MeshChunkConfig cfg = getStaticMeshChunkConfig();
    if (!cfg.enabled)
        return false;

    std::vector<MaterialRange> source_ranges = m_ptr->material_ranges;
    if (source_ranges.empty()) {
        MaterialRange full(0, m_ptr->vertices_len,
                           m_ptr->texture_set ? m_ptr->texture : INVALID_TEXTURE, "");
        full.source_range = 0;
        source_ranges.push_back(full);
    }

    for (size_t i = 0; i < source_ranges.size(); ++i)
        if (source_ranges[i].source_range == std::numeric_limits<size_t>::max())
            source_ranges[i].source_range = i;

    if (m_ptr->transparent && !hasAlphaBlendRange(source_ranges)) {
        for (auto& range : source_ranges)
            range.alpha_mode = 2;
    }

    Assets::ChunkedTriangleMesh chunked = Assets::MeshChunker::buildChunkedIndexedMesh(
        m_ptr->vertices, m_ptr->vertices_len, source_ranges, cfg);
    if (chunked.vertices.empty() || chunked.indices.empty() || chunked.material_ranges.empty())
        return false;

    IGPUMesh* chunked_gpu = render_api->createMesh();
    if (!chunked_gpu)
        return false;

    chunked_gpu->uploadIndexedMeshData(
        chunked.vertices.data(), chunked.vertices.size(),
        chunked.indices.data(), chunked.indices.size());

    if (!chunked_gpu->isUploaded()) {
        delete chunked_gpu;
        return false;
    }

    if (m_ptr->owns_gpu_mesh && m_ptr->gpu_mesh)
        delete m_ptr->gpu_mesh;
    m_ptr->gpu_mesh = chunked_gpu;
    m_ptr->owns_gpu_mesh = true;
    m_ptr->setMaterialRanges(chunked.material_ranges);

    LOG_ENGINE_TRACE("Chunked mesh into {} ranges ({} verts, {} indices)",
                     chunked.material_ranges.size(), chunked.vertices.size(), chunked.indices.size());
    return true;
}

static std::vector<MaterialRange> buildGltfSourceRangesForChunking(const GltfLoadResult& gltf)
{
    std::vector<MaterialRange> ranges;
    ranges.reserve(gltf.primitive_vertex_counts.size());

    size_t current_vertex = 0;
    for (size_t i = 0; i < gltf.primitive_vertex_counts.size(); ++i) {
        MaterialRange range(current_vertex, gltf.primitive_vertex_counts[i], INVALID_TEXTURE, "");
        range.source_range = i;
        if (gltf.materials_loaded && i < gltf.material_indices.size()) {
            int mat_idx = gltf.material_indices[i];
            const GltfMaterial* material = gltf.material_data.getMaterial(mat_idx);
            if (material) {
                range.material_name = material->properties.name;
                if (material->properties.alpha_mode == "MASK")
                    range.alpha_mode = 1;
                else if (material->properties.alpha_mode == "BLEND")
                    range.alpha_mode = 2;
                range.alpha_cutoff = material->properties.alpha_cutoff;
                range.double_sided = material->properties.double_sided;
            }
        }
        ranges.push_back(range);
        current_vertex += gltf.primitive_vertex_counts[i];
    }

    return ranges;
}

static std::vector<MaterialRange> buildFullSourceRangeForChunking(size_t vertex_count)
{
    MaterialRange range(0, vertex_count, INVALID_TEXTURE, "");
    range.source_range = 0;
    return { range };
}

static void prepareChunkedLOD0(MeshPreloadData& data)
{
    Assets::MeshChunkConfig cfg = getStaticMeshChunkConfig();
    if (!cfg.enabled)
        return;

    const vertex* vertices = nullptr;
    size_t vertex_count = 0;
    std::vector<MaterialRange> source_ranges;

    if (data.type == MeshPreloadData::Type::GLTF && data.gltf_geometry) {
        vertices = data.gltf_geometry->vertices;
        vertex_count = data.gltf_geometry->vertex_count;
        source_ranges = buildGltfSourceRangesForChunking(*data.gltf_geometry);
    } else if (data.type == MeshPreloadData::Type::OBJ && data.obj_result) {
        vertices = data.obj_result->vertices;
        vertex_count = data.obj_result->vertex_count;
        source_ranges = buildFullSourceRangeForChunking(vertex_count);
    } else {
        return;
    }

    if (!vertices || vertex_count < 3)
        return;

    auto chunked = std::make_unique<Assets::ChunkedTriangleMesh>(
        Assets::MeshChunker::buildChunkedIndexedMesh(vertices, vertex_count, source_ranges, cfg));

    if (!chunked->vertices.empty() && !chunked->indices.empty() && !chunked->material_ranges.empty())
        data.prepared_chunked_mesh = std::move(chunked);
}

static void prepareChunkedLODs(MeshPreloadData& data)
{
    Assets::MeshChunkConfig cfg = getStaticMeshChunkConfig();
    if (!cfg.enabled || data.lod_mesh_data.empty())
        return;

    std::vector<MaterialRange> material_templates;
    if (data.type == MeshPreloadData::Type::GLTF && data.gltf_geometry)
        material_templates = buildGltfSourceRangesForChunking(*data.gltf_geometry);

    std::vector<bool> split_mask = buildSplitMaskFromRanges(material_templates);
    const std::vector<bool>* split_ptr = split_mask.empty() ? nullptr : &split_mask;

    data.prepared_lod_mesh_data.clear();
    data.prepared_lod_mesh_data.reserve(data.lod_mesh_data.size());
    for (const auto& lod_data : data.lod_mesh_data) {
        if (lod_data.vertices.empty() || lod_data.indices.empty()) {
            data.prepared_lod_mesh_data.push_back(lod_data);
            continue;
        }

        data.prepared_lod_mesh_data.push_back(
            Assets::MeshChunker::chunkLODMesh(lod_data, cfg, split_ptr));
    }
}

static bool uploadPreparedChunkedMaterialMesh(const std::shared_ptr<mesh>& m_ptr,
                                             const Assets::ChunkedTriangleMesh& chunked,
                                             IRenderAPI* render_api)
{
    if (!m_ptr || !render_api || chunked.vertices.empty() ||
        chunked.indices.empty() || chunked.material_ranges.empty())
        return false;

    std::vector<MaterialRange> material_ranges = chunked.material_ranges;
    if (m_ptr->uses_material_ranges) {
        for (auto& range : material_ranges) {
            if (const auto* base_range = findMaterialTemplate(m_ptr->material_ranges, range.source_range))
                copyMaterialPayload(range, *base_range);
        }
    }

    IGPUMesh* chunked_gpu = render_api->createMesh();
    if (!chunked_gpu)
        return false;

    chunked_gpu->uploadIndexedMeshData(
        chunked.vertices.data(), chunked.vertices.size(),
        chunked.indices.data(), chunked.indices.size());

    if (!chunked_gpu->isUploaded()) {
        delete chunked_gpu;
        return false;
    }

    if (m_ptr->owns_gpu_mesh && m_ptr->gpu_mesh)
        delete m_ptr->gpu_mesh;
    m_ptr->gpu_mesh = chunked_gpu;
    m_ptr->owns_gpu_mesh = true;
    m_ptr->setMaterialRanges(material_ranges);

    LOG_ENGINE_TRACE("Uploaded prepared chunked mesh with {} ranges ({} verts, {} indices)",
                     material_ranges.size(), chunked.vertices.size(), chunked.indices.size());
    return true;
}

static void applyLevelMeshProperties(const std::shared_ptr<mesh>& m_ptr, const LevelEntity& entity)
{
    if (!m_ptr)
        return;

    m_ptr->culling = entity.culling;
    m_ptr->transparent = entity.transparent;
    m_ptr->visible = entity.visible;
    m_ptr->force_lod = entity.force_lod;
    m_ptr->updateTransparencyFromMaterials();
}

static std::shared_ptr<mesh> makeMeshInstanceView(const std::shared_ptr<mesh>& resource, const LevelEntity& entity)
{
    if (!resource)
        return nullptr;

    auto instance = std::make_shared<mesh>(resource->vertices, resource->vertices_len);
    instance->owns_vertices = false;
    instance->is_valid = resource->is_valid;
    instance->gpu_mesh = resource->gpu_mesh;
    instance->owns_gpu_mesh = false;
    instance->texture = resource->texture;
    instance->texture_set = resource->texture_set;
    instance->heightmap_displacement = resource->heightmap_displacement;
    instance->heightmap_texture = resource->heightmap_texture;
    instance->heightmap_height_scale = resource->heightmap_height_scale;
    instance->heightmap_height_offset = resource->heightmap_height_offset;
    instance->heightmap_texel_size = resource->heightmap_texel_size;
    instance->material_ranges = resource->material_ranges;
    instance->uses_material_ranges = resource->uses_material_ranges;
    instance->load_state.store(resource->load_state.load(std::memory_order_acquire),
                               std::memory_order_release);
    instance->asset_handle = resource->asset_handle;
    instance->source_path = resource->source_path;
    instance->collision_cache_path = resource->collision_cache_path;
    instance->collision_source_hash = resource->collision_source_hash;
    instance->collision_source_file_size = resource->collision_source_file_size;
    instance->aabb_min = resource->aabb_min;
    instance->aabb_max = resource->aabb_max;
    instance->bounds_computed = resource->bounds_computed;
    instance->current_lod.store(resource->current_lod.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);

    instance->lod_levels.reserve(resource->lod_levels.size());
    for (const auto& resource_lod : resource->lod_levels) {
        mesh::LODLevel lod_view;
        lod_view.gpu_mesh = resource_lod.gpu_mesh;
        lod_view.owns_gpu_mesh = false;
        lod_view.vertex_count = resource_lod.vertex_count;
        lod_view.index_count = resource_lod.index_count;
        lod_view.screen_threshold = resource_lod.screen_threshold;
        lod_view.material_ranges = resource_lod.material_ranges;
        instance->lod_levels.push_back(std::move(lod_view));
    }

    instance->resource_owner = resource;
    applyLevelMeshProperties(instance, entity);
    return instance;
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

void LevelManager::setGameplayDefaults(std::string game_mode_class, std::string game_state_class)
{
    m_default_game_mode_class = game_mode_class.empty() ? "GameMode" : std::move(game_mode_class);
    m_default_game_state_class = game_state_class.empty() ? "GameState" : std::move(game_state_class);
}

void LevelManager::applyGameplayFrameworkSettings(const LevelMetadata& metadata,
                                                  world& game_world,
                                                  const std::string& options,
                                                  bool create_authority_game_mode) const
{
    using namespace GameFramework;

    const std::string game_mode_class =
        resolvedGameModeClass(metadata, m_default_game_mode_class, options);
    const std::string game_state_class =
        resolvedGameStateClass(metadata, m_default_game_state_class, options);

    game_world.shutdownGameplayFramework();

    if (create_authority_game_mode)
    {
        auto game_mode = createGameModeOrFallback(game_mode_class);
        if (!game_mode)
        {
            LOG_ENGINE_WARN("Failed to create GameMode '{}'", game_mode_class);
            game_world.clearGameplayFramework();
            return;
        }

        if (game_mode_class != game_mode->getClassName())
        {
            LOG_ENGINE_WARN("GameMode '{}' is not registered, falling back to '{}'",
                            game_mode_class, game_mode->getClassName());
        }

        auto game_state = createGameStateOrFallback(game_state_class);
        if (!game_state)
        {
            LOG_ENGINE_WARN("Failed to create GameState '{}'", game_state_class);
        }
        else if (game_state_class != game_state->getClassName())
        {
            LOG_ENGINE_WARN("GameState '{}' is not registered, falling back to '{}'",
                            game_state_class, game_state->getClassName());
        }

        game_world.setAuthorityGameMode(std::move(game_mode));
        game_world.setGameState(std::move(game_state));

        if (GameModeBase* game_mode = game_world.getAuthorityGameMode())
            applyGameModeRuntimeSettings(*game_mode, metadata);
        return;
    }

    if (game_world.getAuthorityGameMode())
        game_world.setAuthorityGameMode(nullptr);

    auto game_state = createGameStateOrFallback(game_state_class);
    if (!game_state)
    {
        LOG_ENGINE_WARN("Failed to create client GameState '{}'", game_state_class);
        game_world.setGameState(nullptr);
        return;
    }

    game_world.setGameState(std::move(game_state));

    if (GameStateBase* game_state = game_world.getGameState())
    {
        game_state->setAuthorityGameMode(nullptr);
        game_state->setGameModeClassName(game_mode_class);
    }
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

    // Parse gameplay framework settings
    if (meta.contains("gameplay"))
    {
        const auto& gameplay = meta["gameplay"];
        metadata.game_mode_class = gameplay.value("game_mode_class", metadata.game_mode_class);
        metadata.game_state_class = gameplay.value("game_state_class", metadata.game_state_class);
        metadata.delayed_start = gameplay.value("delayed_start", metadata.delayed_start);
        metadata.start_players_as_spectators =
            gameplay.value("start_players_as_spectators", metadata.start_players_as_spectators);
        metadata.pauseable = gameplay.value("pauseable", metadata.pauseable);
        metadata.min_respawn_delay = gameplay.value("min_respawn_delay", metadata.min_respawn_delay);
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

    // Renderable sidecars are current-format only: a mesh must carry its
    // entity-local visibility/shadow policy instead of inheriting mesh flags.
    if (j.contains("mesh"))
    {
        const auto components = j.find("components");
        if (components == j.end() || !components->is_object() ||
            !components->contains("RenderableComponent") ||
            !(*components)["RenderableComponent"].is_object())
        {
            LOG_ENGINE_ERROR("Level entity '{}' has mesh data without RenderableComponent; rejected", entity.name);
            return false;
        }
        const auto& renderable = (*components)["RenderableComponent"];
        EntityGuid owner_guid{};
        constexpr uint32_t known_render_layers =
            renderLayerMask(RenderLayer::World) |
            renderLayerMask(RenderLayer::ViewModel) |
            renderLayerMask(RenderLayer::OwnerHidden) |
            renderLayerMask(RenderLayer::OwnerOnly);
        if (!renderable.contains("layers") || !renderable["layers"].is_number_unsigned() ||
            (renderable["layers"].get<uint32_t>() & ~known_render_layers) != 0 ||
            !renderable.contains("owner") || !renderable["owner"].is_string() ||
            !guidFromString(renderable["owner"].get<std::string>(), owner_guid) ||
            !renderable.contains("casts_shadow") || !renderable["casts_shadow"].is_boolean())
        {
            LOG_ENGINE_ERROR("Level entity '{}' has an invalid RenderableComponent; rejected", entity.name);
            return false;
        }
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

    if (j.contains("components") && j["components"].is_object())
    {
        entity.reflected_components = j["components"];
        applyReflectedComponentsToLevelEntity(entity.reflected_components, entity);
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
    j["metadata"]["gameplay"]["game_mode_class"] = meta.game_mode_class;
    j["metadata"]["gameplay"]["game_state_class"] = meta.game_state_class;
    j["metadata"]["gameplay"]["delayed_start"] = meta.delayed_start;
    j["metadata"]["gameplay"]["start_players_as_spectators"] = meta.start_players_as_spectators;
    j["metadata"]["gameplay"]["pauseable"] = meta.pauseable;
    j["metadata"]["gameplay"]["min_respawn_delay"] = meta.min_respawn_delay;
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

        if (le.reflected_components.is_object() && !le.reflected_components.empty())
            e["components"] = le.reflected_components;

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
    writeString(file, metadata.game_mode_class);
    writeString(file, metadata.game_state_class);
    uint8_t delayed_start = metadata.delayed_start ? 1 : 0;
    uint8_t start_spectators = metadata.start_players_as_spectators ? 1 : 0;
    uint8_t pauseable = metadata.pauseable ? 1 : 0;
    file.write(reinterpret_cast<const char*>(&delayed_start), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&start_spectators), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&pauseable), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&metadata.min_respawn_delay), sizeof(float));
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

    file.write(reinterpret_cast<const char*>(&has_rigidbody), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&entity.mass), sizeof(float));
    file.write(reinterpret_cast<const char*>(&apply_gravity), sizeof(uint8_t));

    file.write(reinterpret_cast<const char*>(&has_collider), sizeof(uint8_t));
    writeString(file, entity.collider_mesh_path);
    file.write(reinterpret_cast<const char*>(&use_mesh_col), sizeof(uint8_t));

    file.write(reinterpret_cast<const char*>(&culling), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&transparent), sizeof(uint8_t));
    file.write(reinterpret_cast<const char*>(&visible), sizeof(uint8_t));

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

    writeString(file, entity.reflected_components.is_object() ? entity.reflected_components.dump() : "{}");
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
        printf("ERROR: Unsupported binary level version (expected %u, got %u)\n",
               LEVEL_BINARY_VERSION, version);
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
    if (!readString(file, metadata.game_mode_class)) return false;
    if (!readString(file, metadata.game_state_class)) return false;
    uint8_t delayed_start = 0;
    uint8_t start_spectators = 0;
    uint8_t pauseable = 1;
    file.read(reinterpret_cast<char*>(&delayed_start), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&start_spectators), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&pauseable), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&metadata.min_respawn_delay), sizeof(float));
    metadata.delayed_start = delayed_start != 0;
    metadata.start_players_as_spectators = start_spectators != 0;
    metadata.pauseable = pauseable != 0;
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
    uint8_t culling, transparent, visible;

    file.read(reinterpret_cast<char*>(&has_rigidbody), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&entity.mass), sizeof(float));
    file.read(reinterpret_cast<char*>(&apply_gravity), sizeof(uint8_t));

    file.read(reinterpret_cast<char*>(&has_collider), sizeof(uint8_t));
    if (!readString(file, entity.collider_mesh_path)) return false;
    file.read(reinterpret_cast<char*>(&use_mesh_col), sizeof(uint8_t));

    file.read(reinterpret_cast<char*>(&culling), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&transparent), sizeof(uint8_t));
    file.read(reinterpret_cast<char*>(&visible), sizeof(uint8_t));
    if (file.fail()) return false;

    entity.has_rigidbody    = has_rigidbody != 0;
    entity.apply_gravity    = apply_gravity != 0;
    entity.has_collider     = has_collider != 0;
    entity.use_mesh_collision = use_mesh_col != 0;
    entity.culling          = culling != 0;
    entity.transparent      = transparent != 0;
    entity.visible          = visible != 0;

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

    // Current binary records carry all entity fields and reflected components.
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

    std::string reflected_json;
    if (!readString(file, reflected_json)) return false;
    if (!reflected_json.empty())
    {
        json parsed = json::parse(reflected_json, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
            return false;
        entity.reflected_components = std::move(parsed);
        applyReflectedComponentsToLevelEntity(entity.reflected_components, entity);
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
                applyMeshCollisionMetadata(compiled, resolved_path);
                compiled->culling = entity.culling;
                compiled->transparent = entity.transparent;
                compiled->visible = entity.visible;
                compiled->force_lod = entity.force_lod;
                compiled->updateTransparencyFromMaterials();
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

        MaterialLoaderConfig material_config = makeLevelMaterialLoaderConfig(resolved_path, true);

        // Load geometry and materials
        GltfLoadResult map_result = GltfLoader::loadGltfWithMaterials(resolved_path, render_api, gltf_config, material_config);

        if (!map_result.success) {
            LOG_ENGINE_FATAL("Failed to load glTF file: %s\n", map_result.error_message.c_str());
            return nullptr;
        }

        LOG_ENGINE_TRACE("Loaded glTF: {}", resolved_path.c_str());
        
        // Create mesh from glTF data
        m_ptr = std::make_shared<mesh>(map_result.vertices, map_result.vertex_count);
        m_ptr->owns_vertices = true;

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
                    range.source_range = i;
                    // Propagate alpha properties from glTF material
                    if (material.properties.alpha_mode == "MASK")
                        range.alpha_mode = 1;
                    else if (material.properties.alpha_mode == "BLEND")
                        range.alpha_mode = 2;
                    range.alpha_cutoff = material.properties.alpha_cutoff;
                    range.double_sided = material.properties.double_sided;
                    range.metallic_factor = material.properties.metallic_factor;
                    range.roughness_factor = material.properties.roughness_factor;
                    range.emissive_factor = glm::vec3(material.properties.emissive_factor[0], material.properties.emissive_factor[1], material.properties.emissive_factor[2]);
                    range.base_color_factor = glm::vec4(material.properties.base_color_factor[0], material.properties.base_color_factor[1], material.properties.base_color_factor[2], material.properties.base_color_factor[3]);
                    applyWaterMaterialDefaults(range);

                    // Wire PBR texture handles
                    const auto* mr_tex = material.textures.getTexture(TextureType::METALLIC_ROUGHNESS);
                    if (mr_tex && mr_tex->is_loaded) range.metallic_roughness_texture = mr_tex->handle;
                    const auto* nm_tex = material.textures.getTexture(TextureType::NORMAL);
                    if (nm_tex && nm_tex->is_loaded) range.normal_texture = nm_tex->handle;
                    const auto* ao_tex = material.textures.getTexture(TextureType::OCCLUSION);
                    if (ao_tex && ao_tex->is_loaded) range.occlusion_texture = ao_tex->handle;
                    const auto* em_tex = material.textures.getTexture(TextureType::EMISSIVE);
                    if (em_tex && em_tex->is_loaded) range.emissive_texture = em_tex->handle;
                    material_ranges.push_back(range);

                    if (tex != INVALID_TEXTURE) {
                        texture_applied = true;
                    }
                }
                else {
                    MaterialRange range(current_vertex, vertex_count, INVALID_TEXTURE, "unknown");
                    range.source_range = i;
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
        applyMeshCollisionMetadata(m_ptr, resolved_path);
        m_ptr->culling = entity.culling;
        m_ptr->transparent = entity.transparent;
        m_ptr->visible = entity.visible;
        m_ptr->force_lod = entity.force_lod;
        m_ptr->updateTransparencyFromMaterials();

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
                        Assets::LODMeshData lod_upload = maybeChunkLODForRuntime(
                            lod_data, m_ptr->material_ranges, m_ptr->transparent);

                        mesh::LODLevel level;
                        level.screen_threshold = lod_info.screen_threshold;
                        level.vertex_count = lod_upload.vertices.size();
                        level.index_count = lod_upload.indices.size();
                        level.gpu_mesh = render_api->createMesh();
                        if (level.gpu_mesh)
                        {
                            level.gpu_mesh->uploadIndexedMeshData(
                                lod_upload.vertices.data(), lod_upload.vertices.size(),
                                lod_upload.indices.data(), lod_upload.indices.size()
                            );
                        }

                        // Map LOD submesh ranges to original mesh's material textures
                        if (!lod_upload.submesh_ranges.empty() && m_ptr->uses_material_ranges)
                        {
                            for (const auto& sr : lod_upload.submesh_ranges)
                            {
                                MaterialRange lod_range(sr.start_index, sr.index_count, INVALID_TEXTURE, "");
                                if (const auto* base_range = findMaterialTemplate(m_ptr->material_ranges, sr.submesh_id))
                                    copyMaterialPayload(lod_range, *base_range);
                                lod_range.source_range = sr.submesh_id;
                                lod_range.has_bounds = sr.has_bounds;
                                lod_range.aabb_min = sr.aabb_min;
                                lod_range.aabb_max = sr.aabb_max;
                                level.material_ranges.push_back(lod_range);
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

        if (render_api && m_ptr->is_valid && !m_ptr->isUploadedToGPU())
            uploadChunkedMaterialMesh(m_ptr, render_api);
    }

    return m_ptr;
}

static TextureHandle uploadCompiledTextureData(IRenderAPI* render_api,
                                               const Assets::CompiledTextureData& tex_data)
{
    if (!render_api || tex_data.mip_levels.empty())
        return INVALID_TEXTURE;

    std::vector<const uint8_t*> mip_ptrs;
    std::vector<size_t> mip_sizes;
    std::vector<std::pair<int,int>> mip_dims;

    for (const auto& mip : tex_data.mip_levels) {
        mip_ptrs.push_back(mip.data.data());
        mip_sizes.push_back(mip.data.size());
        mip_dims.push_back({static_cast<int>(mip.width), static_cast<int>(mip.height)});
    }

    return render_api->loadCompressedTextureMipRange(
        static_cast<int>(tex_data.header.width),
        static_cast<int>(tex_data.header.height),
        static_cast<uint32_t>(tex_data.header.format),
        static_cast<int>(tex_data.header.mip_count),
        static_cast<int>(tex_data.first_mip),
        mip_ptrs, mip_sizes, mip_dims);
}

// Helper: load a .ctex compiled texture via the render API
static TextureHandle loadCompiledTexture(IRenderAPI* render_api, const std::string& ctex_path)
{
    Assets::CompiledTextureData tex_data;
    if (!loadCompiledTextureDataForStreaming(tex_data, ctex_path))
        return INVALID_TEXTURE;

    return uploadCompiledTextureData(render_api, tex_data);
}

// Helper: load texture with .ctex fallback
static TextureHandle loadTextureWithFallback(IRenderAPI* render_api, const std::string& path)
{
    if (!render_api || path.empty())
        return INVALID_TEXTURE;

    // Try .ctex first
    std::filesystem::path p(path);
    p = p.lexically_normal();
    std::filesystem::path ctex_path = p;
    ctex_path.replace_extension(".ctex");
    if (std::filesystem::exists(ctex_path)) {
        TextureHandle h = loadCompiledTexture(render_api, ctex_path.string());
        if (h != INVALID_TEXTURE) return h;
    }

    // Fallback to original format
    return render_api->loadTexture(path, true, true);
}

using TextureHandleCache = std::unordered_map<std::string, TextureHandle>;

static std::string normalizeTextureCacheKey(const std::string& path)
{
    std::string key = std::filesystem::path(path).lexically_normal().string();
    std::replace(key.begin(), key.end(), '\\', '/');
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

static std::string resolveCompiledTexturePath(const std::filesystem::path& mesh_dir,
                                              const std::string& texture_path)
{
    if (texture_path.empty())
        return {};

    std::filesystem::path path(texture_path);
    if (path.is_absolute())
        return path.lexically_normal().string();

    return (mesh_dir / path).lexically_normal().string();
}

static std::string compiledTexturePathFor(const std::string& texture_path)
{
    std::filesystem::path path(texture_path);
    path.replace_extension(".ctex");
    return path.lexically_normal().string();
}

static TextureHandle loadTextureWithFallbackCached(IRenderAPI* render_api,
                                                   const std::string& path,
                                                   TextureHandleCache& cache)
{
    const std::string key = normalizeTextureCacheKey(path);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    TextureHandle handle = loadTextureWithFallback(render_api, path);
    if (handle != INVALID_TEXTURE)
        cache.emplace(key, handle);
    return handle;
}

static const Assets::CompiledMeshData::MaterialRef* getCompiledMaterialRefForSubmesh(
    const Assets::CompiledMeshData& cmesh,
    size_t submesh_id)
{
    if (submesh_id < cmesh.submeshes.size())
    {
        const auto& submesh = cmesh.submeshes[submesh_id];
        if (submesh.material_index < cmesh.material_refs.size())
            return &cmesh.material_refs[submesh.material_index];
    }

    // Backward compatibility for older compiled meshes that wrote material refs
    // in primitive order instead of using the submesh material index.
    if (cmesh.submeshes.empty() && submesh_id < cmesh.material_refs.size())
        return &cmesh.material_refs[submesh_id];

    return nullptr;
}

template <typename LoadTextureFn>
static void applyCompiledMaterialRef(MaterialRange& range,
                                     const Assets::CompiledMeshData::MaterialRef& mat_ref,
                                     LoadTextureFn&& load_texture)
{
    range.alpha_mode = mat_ref.alpha_mode;
    range.alpha_cutoff = mat_ref.alpha_cutoff;
    range.double_sided = mat_ref.double_sided;
    range.metallic_factor = mat_ref.metallic_factor;
    range.roughness_factor = mat_ref.roughness_factor;
    range.base_color_factor = glm::vec4(mat_ref.base_color_factor[0],
                                        mat_ref.base_color_factor[1],
                                        mat_ref.base_color_factor[2],
                                        mat_ref.base_color_factor[3]);

    TextureHandle first_valid_texture = INVALID_TEXTURE;
    for (const auto& texture_ref : mat_ref.textures)
    {
        TextureHandle texture = load_texture(texture_ref);
        if (texture == INVALID_TEXTURE)
            continue;

        if (first_valid_texture == INVALID_TEXTURE)
            first_valid_texture = texture;

        switch (static_cast<TextureType>(texture_ref.type))
        {
            case TextureType::BASE_COLOR:
            case TextureType::DIFFUSE:
                range.texture = texture;
                break;
            case TextureType::METALLIC_ROUGHNESS:
                range.metallic_roughness_texture = texture;
                break;
            case TextureType::NORMAL:
                range.normal_texture = texture;
                break;
            case TextureType::OCCLUSION:
                range.occlusion_texture = texture;
                break;
            case TextureType::EMISSIVE:
                range.emissive_texture = texture;
                break;
            default:
                break;
        }
    }

    if (range.texture == INVALID_TEXTURE)
        range.texture = first_valid_texture;

    applyWaterMaterialDefaults(range);
}

template <typename LoadTextureFn>
static std::vector<MaterialRange> buildCompiledMaterialRanges(
    const Assets::CompiledMeshData& cmesh,
    const Assets::CompiledMeshData::LODLevel& lod,
    LoadTextureFn&& load_texture)
{
    std::vector<MaterialRange> material_ranges;

    if (!lod.submesh_ranges.empty())
    {
        material_ranges.reserve(lod.submesh_ranges.size());
        for (const auto& sr : lod.submesh_ranges)
        {
            const auto* mat_ref = getCompiledMaterialRefForSubmesh(cmesh, sr.submesh_id);
            const std::string mat_name = mat_ref ? mat_ref->name : "";

            MaterialRange range(sr.start_index, sr.index_count, INVALID_TEXTURE, mat_name);
            range.source_range = sr.submesh_id;
            range.has_bounds = sr.has_bounds;
            range.aabb_min = sr.aabb_min;
            range.aabb_max = sr.aabb_max;

            if (mat_ref)
                applyCompiledMaterialRef(range, *mat_ref, load_texture);

            material_ranges.push_back(range);
        }
        return material_ranges;
    }

    material_ranges.reserve(cmesh.submeshes.size());
    for (size_t submesh_id = 0; submesh_id < cmesh.submeshes.size(); ++submesh_id)
    {
        const auto& submesh = cmesh.submeshes[submesh_id];
        const auto* mat_ref = submesh.material_index < cmesh.material_refs.size()
            ? &cmesh.material_refs[submesh.material_index]
            : nullptr;
        const std::string mat_name = mat_ref ? mat_ref->name : submesh.name;

        MaterialRange range(0, 0, INVALID_TEXTURE, mat_name);
        range.source_range = submesh_id;

        if (mat_ref)
            applyCompiledMaterialRef(range, *mat_ref, load_texture);

        material_ranges.push_back(range);
    }

    return material_ranges;
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
    m_ptr->is_valid = true;

    // Resolve material textures
    const std::filesystem::path mesh_dir = std::filesystem::path(cmesh_path).parent_path();
    if (!cmesh.material_refs.empty() && render_api) {
        TextureHandleCache texture_cache;
        auto load_texture_ref = [&](const Assets::CompiledMeshData::MaterialRef::TextureRef& texture_ref) {
            const std::string texture_path = resolveCompiledTexturePath(mesh_dir, texture_ref.path);
            return loadTextureWithFallbackCached(render_api, texture_path, texture_cache);
        };

        std::vector<MaterialRange> material_ranges =
            buildCompiledMaterialRanges(cmesh, lod0, load_texture_ref);
        if (!material_ranges.empty())
            m_ptr->setMaterialRanges(material_ranges);
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
                MaterialRange lod_range(sr.start_index, sr.index_count, INVALID_TEXTURE, "");
                if (const auto* base_range = findMaterialTemplate(m_ptr->material_ranges, sr.submesh_id))
                    copyMaterialPayload(lod_range, *base_range);
                lod_range.source_range = sr.submesh_id;
                lod_range.has_bounds = sr.has_bounds;
                lod_range.aabb_min = sr.aabb_min;
                lod_range.aabb_max = sr.aabb_max;
                level.material_ranges.push_back(lod_range);
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
    ScopedLoadTimer timer("Mesh CPU preload");

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
            const std::filesystem::path mesh_dir = std::filesystem::path(cmesh_path).parent_path();

            for (const auto& mat_ref : data.compiled_data->material_refs)
            {
                for (const auto& tex_ref : mat_ref.textures)
                {
                    std::string tex_path = resolveCompiledTexturePath(mesh_dir, tex_ref.path);
                    std::string ctex_path = compiledTexturePathFor(tex_path);
                    if (std::filesystem::exists(ctex_path))
                    {
                        const std::string ctex_key = normalizeTextureCacheKey(ctex_path);
                        if (data.preloaded_textures.find(ctex_key) == data.preloaded_textures.end())
                        {
                            MeshPreloadData::PreloadedTexture pt;
                            pt.is_compiled = true;
                            pt.success = loadCompiledTextureDataForStreaming(pt.compiled_tex, ctex_path);
                            data.preloaded_textures[ctex_key] = std::move(pt);
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

        MaterialLoaderConfig material_config = makeLevelMaterialLoaderConfig(resolved_path, false);
        if (!GltfLoader::loadMaterialsIntoResult(*data.gltf_geometry, resolved_path, nullptr, material_config))
        {
            LOG_ENGINE_WARN("Failed to preload glTF material metadata for {}", resolved_path);
        }

        prepareChunkedLOD0(data);
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

        prepareChunkedLOD0(data);
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

        prepareChunkedLODs(data);
    }

    data.success = true;
}

// Helper: upload a preloaded compiled texture to GPU (main thread)
static TextureHandle uploadPreloadedCompiledTexture(IRenderAPI* render_api,
                                                     const Assets::CompiledTextureData& tex_data)
{
    return uploadCompiledTextureData(render_api, tex_data);
}

static TextureHandle loadPreloadedTextureWithFallbackCached(IRenderAPI* render_api,
                                                            MeshPreloadData& preload,
                                                            const std::string& path,
                                                            TextureHandleCache& cache)
{
    const std::string key = normalizeTextureCacheKey(path);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    TextureHandle handle = INVALID_TEXTURE;
    const std::string ctex_key = normalizeTextureCacheKey(compiledTexturePathFor(path));
    auto preloaded_it = preload.preloaded_textures.find(ctex_key);
    if (preloaded_it != preload.preloaded_textures.end() && preloaded_it->second.success)
        handle = uploadPreloadedCompiledTexture(render_api, preloaded_it->second.compiled_tex);

    if (handle == INVALID_TEXTURE)
        handle = loadTextureWithFallback(render_api, path);

    if (handle != INVALID_TEXTURE)
        cache.emplace(key, handle);
    return handle;
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
    applyMeshCollisionMetadata(m_ptr, preload.resolved_path);

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
    m_ptr->is_valid = true;

    // Resolve material textures using preloaded texture data
    const std::filesystem::path mesh_dir = std::filesystem::path(preload.resolved_path).parent_path();
    if (!cmesh.material_refs.empty() && render_api) {
        TextureHandleCache texture_cache;
        auto load_texture_ref = [&](const Assets::CompiledMeshData::MaterialRef::TextureRef& texture_ref) {
            const std::string texture_path = resolveCompiledTexturePath(mesh_dir, texture_ref.path);
            return loadPreloadedTextureWithFallbackCached(render_api, preload, texture_path, texture_cache);
        };

        std::vector<MaterialRange> material_ranges =
            buildCompiledMaterialRanges(cmesh, lod0, load_texture_ref);
        if (!material_ranges.empty())
            m_ptr->setMaterialRanges(material_ranges);
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
                MaterialRange lod_range(sr.start_index, sr.index_count, INVALID_TEXTURE, "");
                if (const auto* base_range = findMaterialTemplate(m_ptr->material_ranges, sr.submesh_id))
                    copyMaterialPayload(lod_range, *base_range);
                lod_range.source_range = sr.submesh_id;
                lod_range.has_bounds = sr.has_bounds;
                lod_range.aabb_min = sr.aabb_min;
                lod_range.aabb_max = sr.aabb_max;
                level.material_ranges.push_back(lod_range);
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
    m_ptr->force_lod = entity.force_lod;
    m_ptr->updateTransparencyFromMaterials();

    return m_ptr;
}

std::shared_ptr<mesh> LevelManager::finalizeMeshGPU(
    MeshPreloadData& preload,
    const LevelEntity& entity,
    IRenderAPI* render_api)
{
    // Main thread only - takes pre-loaded CPU data and does GPU uploads
    ScopedLoadTimer timer("Mesh GPU finalize");
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
        m_ptr->owns_vertices = true;
        // Transfer ownership
        gltf.vertices = nullptr;
        gltf.vertex_count = 0;

        MaterialLoaderConfig material_config = makeLevelMaterialLoaderConfig(preload.resolved_path, false);

        if (gltf.materials_loaded && gltf.material_data.success)
        {
            GltfMaterialLoader::uploadTextures(gltf.material_data, render_api, material_config);
        }
        else
        {
            // Fallback for callers that did not use preloadMeshCPU.
            GltfLoader::loadMaterialsIntoResult(gltf, preload.resolved_path, render_api, material_config);
        }

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
                    range.source_range = i;
                    // Propagate alpha properties from glTF material
                    if (material.properties.alpha_mode == "MASK")
                        range.alpha_mode = 1;
                    else if (material.properties.alpha_mode == "BLEND")
                        range.alpha_mode = 2;
                    range.alpha_cutoff = material.properties.alpha_cutoff;
                    range.double_sided = material.properties.double_sided;
                    range.metallic_factor = material.properties.metallic_factor;
                    range.roughness_factor = material.properties.roughness_factor;
                    range.emissive_factor = glm::vec3(material.properties.emissive_factor[0], material.properties.emissive_factor[1], material.properties.emissive_factor[2]);
                    range.base_color_factor = glm::vec4(material.properties.base_color_factor[0], material.properties.base_color_factor[1], material.properties.base_color_factor[2], material.properties.base_color_factor[3]);
                    applyWaterMaterialDefaults(range);

                    // Wire PBR texture handles
                    const auto* mr_tex = material.textures.getTexture(TextureType::METALLIC_ROUGHNESS);
                    if (mr_tex && mr_tex->is_loaded) range.metallic_roughness_texture = mr_tex->handle;
                    const auto* nm_tex = material.textures.getTexture(TextureType::NORMAL);
                    if (nm_tex && nm_tex->is_loaded) range.normal_texture = nm_tex->handle;
                    const auto* ao_tex = material.textures.getTexture(TextureType::OCCLUSION);
                    if (ao_tex && ao_tex->is_loaded) range.occlusion_texture = ao_tex->handle;
                    const auto* em_tex = material.textures.getTexture(TextureType::EMISSIVE);
                    if (em_tex && em_tex->is_loaded) range.emissive_texture = em_tex->handle;
                    material_ranges.push_back(range);
                    if (tex != INVALID_TEXTURE) texture_applied = true;
                } else {
                    MaterialRange range(current_vertex, vertex_count, INVALID_TEXTURE, "unknown");
                    range.source_range = i;
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
        m_ptr->owns_vertices = true;
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

    applyMeshCollisionMetadata(m_ptr, preload.resolved_path);

    // Apply entity properties
    m_ptr->culling = entity.culling;
    m_ptr->transparent = entity.transparent;
    m_ptr->visible = entity.visible;
    m_ptr->force_lod = entity.force_lod;
    m_ptr->updateTransparencyFromMaterials();

    const bool defer_base_upload_for_chunking = getStaticMeshChunkConfig().enabled;

    // Upload mesh to GPU
    if (render_api && m_ptr->is_valid && !m_ptr->isUploadedToGPU() && !defer_base_upload_for_chunking) {
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

            const size_t current_lod_data_idx = lod_data_idx++;
            auto& lod_data = preload.lod_mesh_data[current_lod_data_idx];
            const bool can_use_prepared_lod =
                !m_ptr->transparent &&
                current_lod_data_idx < preload.prepared_lod_mesh_data.size() &&
                !preload.prepared_lod_mesh_data[current_lod_data_idx].indices.empty();

            Assets::LODMeshData lod_upload = can_use_prepared_lod
                ? preload.prepared_lod_mesh_data[current_lod_data_idx]
                : maybeChunkLODForRuntime(lod_data, m_ptr->material_ranges, m_ptr->transparent);

            mesh::LODLevel level;
            level.screen_threshold = lod_info.screen_threshold;
            level.vertex_count = lod_upload.vertices.size();
            level.index_count = lod_upload.indices.size();
            level.gpu_mesh = render_api->createMesh();
            if (level.gpu_mesh) {
                level.gpu_mesh->uploadIndexedMeshData(
                    lod_upload.vertices.data(), lod_upload.vertices.size(),
                    lod_upload.indices.data(), lod_upload.indices.size());
            }

            // Map LOD submesh ranges to original mesh's material textures
            if (!lod_upload.submesh_ranges.empty() && m_ptr->uses_material_ranges) {
                for (const auto& sr : lod_upload.submesh_ranges) {
                    MaterialRange lod_range(sr.start_index, sr.index_count, INVALID_TEXTURE, "");
                    if (const auto* base_range = findMaterialTemplate(m_ptr->material_ranges, sr.submesh_id))
                        copyMaterialPayload(lod_range, *base_range);
                    lod_range.source_range = sr.submesh_id;
                    lod_range.has_bounds = sr.has_bounds;
                    lod_range.aabb_min = sr.aabb_min;
                    lod_range.aabb_max = sr.aabb_max;
                    level.material_ranges.push_back(lod_range);
                }
            }

            m_ptr->lod_levels.push_back(std::move(level));
        }

        if (!m_ptr->lod_levels.empty()) {
            m_ptr->computeBounds();
            LOG_ENGINE_TRACE("Loaded {} LOD levels for {}", m_ptr->lod_levels.size(), preload.resolved_path);
        }
    }

    if (render_api && m_ptr->is_valid && !m_ptr->isUploadedToGPU()) {
        const bool can_use_prepared_chunking =
            preload.prepared_chunked_mesh &&
            !m_ptr->transparent &&
            !hasAlphaBlendRange(m_ptr->material_ranges);

        if (can_use_prepared_chunking) {
            if (!uploadPreparedChunkedMaterialMesh(m_ptr, *preload.prepared_chunked_mesh, render_api))
                m_ptr->uploadToGPU(render_api);
        } else if (!uploadChunkedMaterialMesh(m_ptr, render_api)) {
            m_ptr->uploadToGPU(render_api);
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
    entt::entity* out_player_rep_entity,
    bool create_authority_game_mode,
    std::vector<entt::entity>* out_created_entities)
{
    LevelInstantiationPlan instantiation_plan;
    if (!prepareLevelInstantiation(level_data, game_world.registry,
                                  instantiation_plan))
        return false;

    LOG_ENGINE_INFO("Instantiating level (parallel): {}", level_data.metadata.level_name);
    ScopedLoadTimer timer("Level instantiation");

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

    // Restore the entire identity cohort before changing settings, publishing
    // outputs, or giving entities to feature services. A failed restore leaves
    // the destination and caller-owned outputs untouched.
    std::unordered_map<EntityGuid, size_t, GuidHash> cohort_indices;
    cohort_indices.reserve(instantiation_plan.identity_guids.size());
    for (size_t i = 0; i < instantiation_plan.identity_guids.size(); ++i)
        cohort_indices.emplace(instantiation_plan.identity_guids[i], i);

    std::vector<entt::entity> created_entities(level_data.entities.size());
    for (auto& entity : created_entities)
        entity = entt::null;
    std::vector<size_t> identity_occurrences(instantiation_plan.identity_guids.size(), 0);
    std::vector<garden::ecs::EntityHandle> restored_handles;
    std::vector<EntityRef> references;
    references.reserve(instantiation_plan.identity_guids.size());
    for (const EntityGuid& guid : instantiation_plan.identity_guids)
        references.push_back(EntityRef{guid});
    if (!game_world.restoreScopedEntities(references, restored_handles))
        return false;
    const size_t restored_count = restored_handles.size();

    // Internal legacy bridge: take one snapshot for the level component and
    // physics passes, which still use raw entities. Scoped restore owns identity
    // construction; this lookup does not create or alter identity.
    bool valid_cohort = restored_count == instantiation_plan.identity_guids.size();
    for (auto [entity, identity] : game_world.registry.view<IdentityComponent>().each())
    {
        const auto it = cohort_indices.find(identity.guid);
        if (it != cohort_indices.end())
        {
            ++identity_occurrences[it->second];
            if (it->second < restored_count)
                created_entities[it->second] = entity;
        }
    }
    for (size_t i = 0; i < created_entities.size(); ++i)
    {
        if (identity_occurrences[i] != 1 || created_entities[i] == entt::null)
            valid_cohort = false;
    }

    if (!valid_cohort)
    {
        // Retire by scoped handle even if a construct hook removed or changed
        // identity; looking up by the expected GUID would miss that entity.
        for (auto it = restored_handles.rbegin(); it != restored_handles.rend(); ++it)
        {
            if (game_world.destroyEntity(*it) != garden::ecs::ComponentStoreResult::Success)
                LOG_ENGINE_ERROR("Level instantiation: failed to retire a restored entity");
        }
        LOG_ENGINE_ERROR("Level instantiation rejected: failed to restore scoped identity");
        return false;
    }

    const auto previous_gravity = game_world.getGravity();
    const auto previous_delta = game_world.getFixedDelta();
    try {

    entt::entity player_entity = entt::null;
    entt::entity freecam_entity = entt::null;
    entt::entity player_rep_entity = entt::null;

    // Map to store entities by name for reference resolution
    std::map<std::string, entt::entity> entity_map;

    // Cache finalized resources to avoid duplicate GPU uploads for same path.
    // Entity components receive lightweight instance views so per-entity render
    // state does not leak between users of the same mesh asset.
    std::unordered_map<std::string, std::shared_ptr<mesh>> finalized_meshes;

    // Helper to get or finalize a mesh from the preload cache
    auto getOrFinalizeMesh = [&](const std::string& mesh_path, const LevelEntity& ent) -> std::shared_ptr<mesh> {
        if (mesh_path.empty()) return nullptr;
        std::string resolved = Assets::AssetManager::get().resolveAssetPath(mesh_path);

        // Check if already finalized (GPU-level dedup)
        auto fin_it = finalized_meshes.find(resolved);
        if (fin_it != finalized_meshes.end()) {
            return makeMeshInstanceView(fin_it->second, ent);
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
            return makeMeshInstanceView(mesh_ptr, ent);
        }
        return nullptr;
    };

    // Create all entities (same logic as instantiateLevel, using preloaded data)
    for (size_t entity_index = 0; entity_index < level_data.entities.size(); ++entity_index)
    {
        const auto& entity_data = level_data.entities[entity_index];
        const auto e = created_entities[entity_index];

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

        // Add Physics components
        if (entity_data.type == EntityType::Physical ||
            entity_data.type == EntityType::Player)
        {
            if (entity_data.has_rigidbody || entity_data.type == EntityType::Physical) {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = entity_data.mass;
                rb.apply_gravity = entity_data.apply_gravity;
                rb.motion_type = stringToBodyMotionType(entity_data.body_motion_type);
            }
        }

        setupLevelColliderComponent(game_world.registry, e, entity_data, getOrFinalizeMesh);
        deserializeReflectedComponents(m_reflection, game_world.registry, e,
                                       instantiation_plan.reflected_components[entity_index]);
        if (!setupLevelTerrainComponent(game_world.registry, e, entity_data, render_api))
            throw std::runtime_error("terrain construction failed");
        applyWaterComponentToEntityMesh(game_world.registry, e);

        // Player
        if (entity_data.type == EntityType::Player)
        {
            auto& pc = game_world.registry.get_or_emplace<PlayerComponent>(e);
            pc.speed = entity_data.speed;
            pc.jump_force = entity_data.jump_force;
            pc.mouse_sensitivity = entity_data.mouse_sensitivity;

            auto& cc = game_world.registry.get_or_emplace<CharacterControllerComponent>(e);
            cc.move_speed = entity_data.speed;
            cc.jump_velocity = entity_data.jump_force;
            cc.input_enabled = pc.input_enabled;
            cc.capsule_half_height = pc.capsule_half_height;
            cc.capsule_radius = pc.capsule_radius;

            if (!game_world.registry.all_of<RigidBodyComponent>(e)) {
                game_world.registry.emplace<RigidBodyComponent>(e);
                auto& rb = game_world.registry.get<RigidBodyComponent>(e);
                rb.mass = 80.0f;
                rb.apply_gravity = false;
            }

            {
                game_world.getPhysicsSystem().createPlayerBody(game_world.registry, e);
                if (!game_world.getPhysicsSystem().hasCharacterController(e))
                    throw std::runtime_error("player controller construction failed");
            }

            player_entity = e;
        }

        // Freecam
        if (entity_data.type == EntityType::Freecam)
        {
            auto& fc = game_world.registry.get_or_emplace<FreecamComponent>(e);
            fc.movement_speed = entity_data.movement_speed;
            fc.fast_movement_speed = entity_data.fast_movement_speed;
            fc.mouse_sensitivity = entity_data.mouse_sensitivity;

            freecam_entity = e;
        }

        // Player Rep
        if (entity_data.type == EntityType::PlayerRep)
        {
            auto& pr = game_world.registry.get_or_emplace<PlayerRepresentationComponent>(e);
            pr.position_offset = entity_data.position_offset;

            player_rep_entity = e;
        }

        if (entity_data.type == EntityType::PointLight)
        {
            auto& pl = game_world.registry.get_or_emplace<PointLightComponent>(e);
            pl.color = entity_data.light_color;
            pl.intensity = entity_data.light_intensity;
            pl.range = entity_data.light_range;
            pl.constant_attenuation = entity_data.light_constant_attenuation;
            pl.linear_attenuation = entity_data.light_linear_attenuation;
            pl.quadratic_attenuation = entity_data.light_quadratic_attenuation;
        }

        if (entity_data.type == EntityType::SpotLight)
        {
            auto& sl = game_world.registry.get_or_emplace<SpotLightComponent>(e);
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
            auto& cc = game_world.registry.get_or_emplace<ConstraintComponent>(e);
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

    // Restore saved hierarchy links only after the entire LevelData cohort
    // has its final identities. The preflight has removed null/external refs
    // and rejected malformed, self-parented, or cyclic graphs.
    for (size_t i = 0; i < created_entities.size(); ++i)
    {
        const size_t parent_index = instantiation_plan.parent_indices[i];
        if (parent_index == std::numeric_limits<size_t>::max())
            continue;
        game_world.registry.emplace_or_replace<ParentComponent>(
            created_entities[i],
            ParentComponent{EntityRef{instantiation_plan.identity_guids[parent_index]}});
    }
    Hierarchy::rebuildChildren(game_world.registry, created_entities);

    // Kinematic hierarchy children must be created from their composed world
    // pose. Creating every non-player level body here also keeps the previous
    // creation path while ensuring links exist before that pose is sampled.
    // Player CharacterVirtual entities are roots by the staged policy and are
    // created during the entity pass above.
    for (size_t i = 0; i < created_entities.size(); ++i)
        if (level_data.entities[i].type != EntityType::Player &&
            game_world.registry.all_of<ColliderComponent, TransformComponent>(created_entities[i]) &&
            !createLevelPhysicsBody(game_world, created_entities[i], level_data.entities[i]))
            throw std::runtime_error("physics body construction failed");

    // Second pass: Resolve references (PlayerRepresentation)
    for (size_t i = 0; i < level_data.entities.size(); ++i) {
        const auto& entity_data = level_data.entities[i];
        if (entity_data.type == EntityType::PlayerRep) {
            entt::entity e = created_entities[i];
            auto& pr = game_world.registry.get<PlayerRepresentationComponent>(e);

            if (!entity_data.tracked_player_name.empty()) {
                auto it = entity_map.find(entity_data.tracked_player_name);
                if (it != entity_map.end()) {
                    pr.tracked_player = ensureEntityRef(game_world.registry, it->second);
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
                    cc.target_entity = ensureEntityRef(game_world.registry, target_it->second);
                    if (!game_world.getPhysicsSystem().createConstraint(e, target_it->second, cc))
                        throw std::runtime_error("constraint construction failed");
                } else {
                    LOG_ENGINE_WARN("Constraint on '{}' cannot find target '{}'",
                                    entity_data.name, entity_data.constraint_target_name);
                }
            }
        }
    }

    game_world.setGravity(level_data.metadata.gravity);
    game_world.setFixedDelta(level_data.metadata.fixed_delta);
    applyGameplayFrameworkSettings(level_data.metadata, game_world, "", create_authority_game_mode);

    if (out_player_entity) *out_player_entity = player_entity;
    if (out_freecam_entity) *out_freecam_entity = freecam_entity;
    if (out_player_rep_entity) *out_player_rep_entity = player_rep_entity;
    if (out_created_entities) *out_created_entities = std::move(created_entities);

    LOG_ENGINE_INFO("Level instantiation complete (parallel): {} entities", level_data.entities.size());
    return true;
    } catch (...) {
        // Constraints must be removed before any of their bodies.
        for (auto e : created_entities)
            game_world.getPhysicsSystem().removeConstraint(e);
        for (auto e : created_entities)
            game_world.getPhysicsSystem().removeBody(e);
        for (auto it = restored_handles.rbegin(); it != restored_handles.rend(); ++it)
            if (game_world.destroyEntity(*it) != garden::ecs::ComponentStoreResult::Success)
                LOG_ENGINE_ERROR("Level instantiation: failed to retire a restored entity");
        game_world.setGravity(previous_gravity);
        game_world.setFixedDelta(previous_delta);
        LOG_ENGINE_ERROR("Level instantiation failed during construction");
        return false;
    }
}

bool LevelManager::instantiateLevel(
    const LevelData& level_data,
    world& game_world,
    IRenderAPI* render_api,
    entt::entity* out_player_entity,
    entt::entity* out_freecam_entity,
    entt::entity* out_player_rep_entity,
    bool create_authority_game_mode,
    std::vector<entt::entity>* out_created_entities)
{
    return instantiateLevelParallel(
        level_data,
        game_world,
        render_api,
        out_player_entity,
        out_freecam_entity,
        out_player_rep_entity,
        create_authority_game_mode,
        out_created_entities);
}
