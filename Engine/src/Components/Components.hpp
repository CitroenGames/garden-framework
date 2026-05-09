#pragma once
#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <memory>
#include <entt/entt.hpp>
#include "Character/CharacterController.hpp"
#include "mesh.hpp"
#include "Reflection/Reflector.hpp"
#include <algorithm>

struct TransformComponent {
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;

    TransformComponent(float x=0, float y=0, float z=0) : position(x,y,z), rotation(0,0,0), scale(1,1,1) {}

    glm::mat4 getTransformMatrix() const {
        glm::mat4 scale_matrix = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 rotation_matrix = glm::eulerAngleYXZ(
            glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
        glm::mat4 translation_matrix = glm::translate(glm::mat4(1.0f), position);

        return translation_matrix * rotation_matrix * scale_matrix;
    }

    static void reflect(Reflector<TransformComponent>& r) {
        r.display("Transform").category("Core").removable(false);
        r.field<&TransformComponent::position>("position")
            .tooltip("World position").drag(0.01f).category("Transform");
        r.field<&TransformComponent::rotation>("rotation")
            .tooltip("Euler rotation (degrees)").drag(0.5f).category("Transform");
        r.field<&TransformComponent::scale>("scale")
            .tooltip("Scale").drag(0.01f).range(0.001f, 1000.0f).category("Transform");
    }
};

struct TagComponent {
    std::string name;

    static void reflect(Reflector<TagComponent>& r) {
        r.display("Tag").category("Core").removable(false);
        r.field<&TagComponent::name>("name").category("Tag");
    }
};

struct MeshComponent {
    std::shared_ptr<mesh> m_mesh;
    // No reflect() — contains non-reflectable shared_ptr data, handled with custom UI
};

struct TerrainComponent {
    std::string heightmap_path;
    std::string albedo_path;
    float width = 64.0f;
    float depth = 64.0f;
    float height_scale = 8.0f;
    float height_offset = 0.0f;
    int sample_step = 1;
    bool gpu_displacement = true;
    bool generate_collision = true;
    float friction = 0.7f;
    float restitution = 0.0f;

    static void reflect(Reflector<TerrainComponent>& r) {
        r.display("Terrain").category("Rendering");
        r.field<&TerrainComponent::heightmap_path>("heightmap_path")
            .tooltip("Grayscale heightmap asset path").category("Assets").assetPath();
        r.field<&TerrainComponent::albedo_path>("albedo_path")
            .tooltip("Terrain albedo texture asset path").category("Assets").assetPath();
        r.field<&TerrainComponent::width>("width")
            .tooltip("Terrain width in local X units").drag(0.1f).range(0.001f, 100000.0f).category("Size");
        r.field<&TerrainComponent::depth>("depth")
            .tooltip("Terrain depth in local Z units").drag(0.1f).range(0.001f, 100000.0f).category("Size");
        r.field<&TerrainComponent::height_scale>("height_scale")
            .tooltip("Height range applied to normalized heightmap samples").drag(0.1f).range(-100000.0f, 100000.0f).category("Height");
        r.field<&TerrainComponent::height_offset>("height_offset")
            .tooltip("Offset added to generated terrain heights").drag(0.1f).range(-100000.0f, 100000.0f).category("Height");
        r.field<&TerrainComponent::sample_step>("sample_step")
            .tooltip("Heightmap texel stride used for generated terrain vertices").drag(1.0f).range(1.0f, 1024.0f).category("Sampling");
        r.field<&TerrainComponent::gpu_displacement>("gpu_displacement")
            .tooltip("Use backend vertex displacement when supported").category("Rendering");
        r.field<&TerrainComponent::generate_collision>("generate_collision")
            .tooltip("Create a static collision mesh from the heightmap").category("Physics");
        r.field<&TerrainComponent::friction>("friction")
            .tooltip("Terrain collision friction").drag(0.01f).range(0.0f, 10.0f).category("Physics");
        r.field<&TerrainComponent::restitution>("restitution")
            .tooltip("Terrain collision bounciness").drag(0.01f).range(0.0f, 1.0f).category("Physics");
    }
};

enum class BodyMotionType : int
{
    Dynamic = 0,
    Kinematic,
    Static,
    COUNT
};

static const char* body_motion_type_names[] = { "Dynamic", "Kinematic", "Static" };

inline BodyMotionType stringToBodyMotionType(const std::string& s)
{
    if (s == "Kinematic") return BodyMotionType::Kinematic;
    if (s == "Static")    return BodyMotionType::Static;
    return BodyMotionType::Dynamic;
}

inline std::string bodyMotionTypeToString(BodyMotionType t)
{
    switch (t)
    {
    case BodyMotionType::Kinematic: return "Kinematic";
    case BodyMotionType::Static:    return "Static";
    default:                        return "Dynamic";
    }
}

struct RigidBodyComponent {
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 force = glm::vec3(0.0f);
    float mass = 1.0f;
    bool apply_gravity = true;
    BodyMotionType motion_type = BodyMotionType::Dynamic;

    static void reflect(Reflector<RigidBodyComponent>& r) {
        r.display("Rigid Body").category("Physics");
        r.field<&RigidBodyComponent::motion_type>("motion_type")
            .tooltip("Body motion type")
            .enumValues(body_motion_type_names, (int)BodyMotionType::COUNT)
            .category("Physics");
        r.field<&RigidBodyComponent::velocity>("velocity")
            .visible().category("Physics");
        r.field<&RigidBodyComponent::force>("force")
            .visible().category("Physics");
        r.field<&RigidBodyComponent::mass>("mass")
            .tooltip("Body mass").drag(0.1f).range(0.001f, 100000.0f).category("Physics");
        r.field<&RigidBodyComponent::apply_gravity>("apply_gravity")
            .category("Physics");
    }
};

enum class ColliderShapeType : int
{
    Mesh = 0,       // Triangle mesh (static only)
    Box,            // Box with half extents
    Sphere,         // Sphere with radius
    Capsule,        // Capsule with half height and radius
    Cylinder,       // Cylinder with half height and radius
    ConvexHull,     // Convex hull from mesh vertices
    COUNT
};

static const char* collider_shape_type_names[] = {
    "Mesh", "Box", "Sphere", "Capsule", "Cylinder", "ConvexHull"
};

inline ColliderShapeType stringToColliderShapeType(const std::string& s)
{
    if (s == "Box")       return ColliderShapeType::Box;
    if (s == "Sphere")    return ColliderShapeType::Sphere;
    if (s == "Capsule")   return ColliderShapeType::Capsule;
    if (s == "Cylinder")  return ColliderShapeType::Cylinder;
    if (s == "ConvexHull")return ColliderShapeType::ConvexHull;
    return ColliderShapeType::Mesh;
}

inline std::string colliderShapeTypeToString(ColliderShapeType t)
{
    switch (t)
    {
    case ColliderShapeType::Box:       return "Box";
    case ColliderShapeType::Sphere:    return "Sphere";
    case ColliderShapeType::Capsule:   return "Capsule";
    case ColliderShapeType::Cylinder:  return "Cylinder";
    case ColliderShapeType::ConvexHull:return "ConvexHull";
    default:                           return "Mesh";
    }
}

struct ColliderComponent {
    std::shared_ptr<mesh> m_mesh;  // Used for Mesh and ConvexHull shape types

    ColliderShapeType shape_type = ColliderShapeType::Mesh;

    // Primitive shape parameters
    glm::vec3 box_half_extents = glm::vec3(0.5f);
    float sphere_radius = 0.5f;
    float capsule_half_height = 0.5f;
    float capsule_radius = 0.3f;
    float cylinder_half_height = 0.5f;
    float cylinder_radius = 0.5f;

    // Physics material
    float friction = 0.2f;
    float restitution = 0.0f;

    bool is_mesh_valid() const
    {
        return m_mesh != nullptr && m_mesh->is_valid;
    }

    mesh* get_mesh() const
    {
        return (m_mesh != nullptr && m_mesh->is_valid) ? m_mesh.get() : nullptr;
    }

    static void reflect(Reflector<ColliderComponent>& r) {
        r.display("Collider").category("Physics");
        r.field<&ColliderComponent::shape_type>("shape_type")
            .tooltip("Collision shape type")
            .enumValues(collider_shape_type_names, (int)ColliderShapeType::COUNT)
            .category("Shape");
        r.field<&ColliderComponent::box_half_extents>("box_half_extents")
            .tooltip("Box half extents").drag(0.01f).category("Shape");
        r.field<&ColliderComponent::sphere_radius>("sphere_radius")
            .tooltip("Sphere radius").drag(0.01f).range(0.001f, 1000.0f).category("Shape");
        r.field<&ColliderComponent::capsule_half_height>("capsule_half_height")
            .tooltip("Capsule half height").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.field<&ColliderComponent::capsule_radius>("capsule_radius")
            .tooltip("Capsule radius").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.field<&ColliderComponent::cylinder_half_height>("cylinder_half_height")
            .tooltip("Cylinder half height").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.field<&ColliderComponent::cylinder_radius>("cylinder_radius")
            .tooltip("Cylinder radius").drag(0.01f).range(0.001f, 100.0f).category("Shape");
        r.field<&ColliderComponent::friction>("friction")
            .tooltip("Surface friction").drag(0.01f).range(0.0f, 10.0f).category("Material");
        r.field<&ColliderComponent::restitution>("restitution")
            .tooltip("Bounciness").drag(0.01f).range(0.0f, 1.0f).category("Material");
    }
};

struct CharacterControllerComponent {
    float move_speed = 10.0f;
    float jump_velocity = 5.0f;
    float gravity_scale = 1.0f;
    float ground_control = 0.8f;
    float air_control = 0.3f;
    float ground_acceleration = 5.5f;
    float air_acceleration = 12.0f;
    float friction = 5.2f;
    float stop_speed_ratio = 0.25f;
    float air_wish_speed_cap_ratio = 30.0f / 320.0f;
    float surface_friction = 1.0f;
    float capsule_half_height = 0.9f;
    float capsule_radius = 0.3f;
    float mass = 80.0f;
    float max_strength = 100.0f;
    float max_slope_angle = 50.0f;
    float character_padding = 0.02f;
    float collision_tolerance = 0.05f;
    float step_up_height = 0.4f;
    float stick_to_floor_distance = 0.5f;
    bool enhanced_internal_edge_removal = true;
    bool use_inner_body = true;
    bool input_enabled = true;

    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0, 1, 0);
    glm::vec3 ground_velocity = glm::vec3(0);
    bool swimming = false;
    int water_level = 0;

    static void reflect(Reflector<CharacterControllerComponent>& r) {
        r.display("Character Controller").category("Physics");
        r.field<&CharacterControllerComponent::move_speed>("move_speed")
            .tooltip("Maximum movement speed").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&CharacterControllerComponent::jump_velocity>("jump_velocity")
            .tooltip("Upward jump velocity").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&CharacterControllerComponent::gravity_scale>("gravity_scale")
            .tooltip("Gravity multiplier").drag(0.05f).range(0.0f, 10.0f).category("Movement");
        r.field<&CharacterControllerComponent::ground_control>("ground_control")
            .tooltip("Legacy blend movement authority").drag(0.01f).range(0.0f, 1.0f).category("Movement");
        r.field<&CharacterControllerComponent::air_control>("air_control")
            .tooltip("Legacy blend air-control authority").drag(0.01f).range(0.0f, 1.0f).category("Movement");
        r.field<&CharacterControllerComponent::ground_acceleration>("ground_acceleration")
            .tooltip("Source-style ground acceleration").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&CharacterControllerComponent::air_acceleration>("air_acceleration")
            .tooltip("Source-style air acceleration").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&CharacterControllerComponent::friction>("friction")
            .tooltip("Source-style ground friction").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&CharacterControllerComponent::stop_speed_ratio>("stop_speed_ratio")
            .tooltip("Ground stop speed as a fraction of move_speed").drag(0.01f).range(0.0f, 10.0f).category("Movement");
        r.field<&CharacterControllerComponent::air_wish_speed_cap_ratio>("air_wish_speed_cap_ratio")
            .tooltip("Air wish-speed cap as a fraction of move_speed").drag(0.01f).range(0.0f, 10.0f).category("Movement");
        r.field<&CharacterControllerComponent::surface_friction>("surface_friction")
            .tooltip("Movement friction multiplier for the current surface").drag(0.01f).range(0.0f, 10.0f).category("Movement");
        r.field<&CharacterControllerComponent::capsule_half_height>("capsule_half_height")
            .tooltip("Capsule cylinder half height").drag(0.01f).range(0.01f, 10.0f).category("Shape");
        r.field<&CharacterControllerComponent::capsule_radius>("capsule_radius")
            .tooltip("Capsule radius").drag(0.01f).range(0.01f, 5.0f).category("Shape");
        r.field<&CharacterControllerComponent::mass>("mass")
            .tooltip("Character mass").drag(0.1f).range(0.001f, 100000.0f).category("Physics");
        r.field<&CharacterControllerComponent::max_strength>("max_strength")
            .tooltip("Maximum push force").drag(1.0f).range(0.0f, 100000.0f).category("Physics");
        r.field<&CharacterControllerComponent::max_slope_angle>("max_slope_angle")
            .tooltip("Maximum walkable slope angle in degrees").drag(0.5f).range(0.0f, 89.0f).category("Physics");
        r.field<&CharacterControllerComponent::character_padding>("character_padding")
            .tooltip("Distance kept from collision geometry").drag(0.001f).range(0.0f, 0.2f).category("Physics");
        r.field<&CharacterControllerComponent::collision_tolerance>("collision_tolerance")
            .tooltip("Collision separation tolerance").drag(0.001f).range(0.0f, 0.5f).category("Physics");
        r.field<&CharacterControllerComponent::step_up_height>("step_up_height")
            .tooltip("Maximum stair step height").drag(0.01f).range(0.0f, 2.0f).category("Physics");
        r.field<&CharacterControllerComponent::stick_to_floor_distance>("stick_to_floor_distance")
            .tooltip("Distance used to stay snapped to ground").drag(0.01f).range(0.0f, 2.0f).category("Physics");
        r.field<&CharacterControllerComponent::enhanced_internal_edge_removal>("enhanced_internal_edge_removal")
            .category("Physics");
        r.field<&CharacterControllerComponent::use_inner_body>("use_inner_body")
            .tooltip("Expose a kinematic proxy body to raycasts and contacts").category("Physics");
        r.field<&CharacterControllerComponent::input_enabled>("input_enabled")
            .category("Input");
        r.field<&CharacterControllerComponent::grounded>("grounded")
            .visible().category("State");
        r.field<&CharacterControllerComponent::ground_normal>("ground_normal")
            .visible().category("State");
        r.field<&CharacterControllerComponent::ground_velocity>("ground_velocity")
            .visible().category("State");
        r.field<&CharacterControllerComponent::swimming>("swimming")
            .visible().category("State");
        r.field<&CharacterControllerComponent::water_level>("water_level")
            .visible().category("State");
    }
};

struct WaterVolumeComponent {
    glm::vec3 half_extents = glm::vec3(5.0f, 2.0f, 5.0f);
    float surface_offset = 0.0f;
    float swim_speed_scale = 0.8f;
    float water_acceleration = 5.5f;
    float water_friction = 5.2f;
    float water_sink_speed = 1.75f;
    bool enabled = true;

    static void reflect(Reflector<WaterVolumeComponent>& r) {
        r.display("Water Volume").category("Physics");
        r.field<&WaterVolumeComponent::half_extents>("half_extents")
            .tooltip("Water volume half extents in local units").drag(0.1f).category("Volume");
        r.field<&WaterVolumeComponent::surface_offset>("surface_offset")
            .tooltip("Extra water surface height above the volume top").drag(0.01f).range(-10.0f, 10.0f).category("Volume");
        r.field<&WaterVolumeComponent::swim_speed_scale>("swim_speed_scale")
            .tooltip("Fraction of move_speed used while swimming").drag(0.01f).range(0.0f, 2.0f).category("Movement");
        r.field<&WaterVolumeComponent::water_acceleration>("water_acceleration")
            .tooltip("Source-style acceleration while swimming").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&WaterVolumeComponent::water_friction>("water_friction")
            .tooltip("Velocity damping while swimming").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&WaterVolumeComponent::water_sink_speed>("water_sink_speed")
            .tooltip("Downward idle drift while not swimming or jumping").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&WaterVolumeComponent::enabled>("enabled")
            .category("Volume");
    }
};

struct WaterComponent {
    bool enabled = true;
    bool affects_rendering = true;
    bool affects_swimming = true;
    glm::vec3 half_extents = glm::vec3(5.0f, 2.0f, 5.0f);
    float surface_offset = 0.0f;
    float swim_speed_scale = 0.8f;
    float water_acceleration = 5.5f;
    float water_friction = 5.2f;
    float water_sink_speed = 1.75f;

    static void reflect(Reflector<WaterComponent>& r) {
        r.display("Water").category("Rendering");
        r.field<&WaterComponent::enabled>("enabled")
            .category("Water");
        r.field<&WaterComponent::affects_rendering>("affects_rendering")
            .tooltip("Force attached mesh material ranges through the built-in water shader path").category("Rendering");
        r.field<&WaterComponent::affects_swimming>("affects_swimming")
            .tooltip("Use this component as a swimming volume").category("Physics");
        r.field<&WaterComponent::half_extents>("half_extents")
            .tooltip("Water volume half extents in local units").drag(0.1f).category("Volume");
        r.field<&WaterComponent::surface_offset>("surface_offset")
            .tooltip("Extra water surface height above the volume top").drag(0.01f).range(-10.0f, 10.0f).category("Volume");
        r.field<&WaterComponent::swim_speed_scale>("swim_speed_scale")
            .tooltip("Fraction of move_speed used while swimming").drag(0.01f).range(0.0f, 2.0f).category("Movement");
        r.field<&WaterComponent::water_acceleration>("water_acceleration")
            .tooltip("Source-style acceleration while swimming").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&WaterComponent::water_friction>("water_friction")
            .tooltip("Velocity damping while swimming").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&WaterComponent::water_sink_speed>("water_sink_speed")
            .tooltip("Downward idle drift while not swimming or jumping").drag(0.1f).range(0.0f, 100.0f).category("Movement");
    }
};

inline void applyWaterComponentToMesh(const WaterComponent& water, mesh& m)
{
    if (!water.enabled || !water.affects_rendering)
        return;

    if (m.uses_material_ranges && !m.material_ranges.empty())
    {
        for (auto& range : m.material_ranges)
        {
            range.material_flags |= MaterialFlags::Water;
            range.double_sided = true;
            range.metallic_factor = 0.0f;
            range.roughness_factor = std::min(range.roughness_factor, 0.08f);
            range.emissive_factor = glm::max(range.emissive_factor, glm::vec3(0.0f, 0.025f, 0.04f));
        }

        for (auto& lod : m.lod_levels)
        {
            for (auto& range : lod.material_ranges)
            {
                range.material_flags |= MaterialFlags::Water;
                range.double_sided = true;
                range.metallic_factor = 0.0f;
                range.roughness_factor = std::min(range.roughness_factor, 0.08f);
                range.emissive_factor = glm::max(range.emissive_factor, glm::vec3(0.0f, 0.025f, 0.04f));
            }
        }
        return;
    }

    if (!m.is_valid || m.vertices_len == 0)
        return;

    MaterialRange range(0, m.vertices_len, m.texture_set ? m.texture : INVALID_TEXTURE, "WaterComponent");
    range.material_flags = MaterialFlags::Water;
    range.double_sided = true;
    range.metallic_factor = 0.0f;
    range.roughness_factor = 0.055f;
    range.base_color_factor = glm::vec4(0.05f, 0.32f, 0.55f, 1.0f);
    range.emissive_factor = glm::vec3(0.0f, 0.025f, 0.04f);
    m.setMaterialRanges({range});
}

struct PlayerComponent {
    float speed = 1.5f;
    float jump_force = 3.0f;
    float mouse_sensitivity = 1.0f;
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0, 1, 0);
    bool input_enabled = true;
    float capsule_half_height = 0.9f;
    float capsule_radius = 0.3f;

    static void reflect(Reflector<PlayerComponent>& r) {
        r.display("Player").category("Gameplay");
        r.field<&PlayerComponent::speed>("speed")
            .tooltip("Movement speed").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&PlayerComponent::jump_force>("jump_force")
            .tooltip("Jump force").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&PlayerComponent::mouse_sensitivity>("mouse_sensitivity")
            .tooltip("Mouse sensitivity").drag(0.01f).range(0.01f, 10.0f).category("Input");
        r.field<&PlayerComponent::grounded>("grounded")
            .visible().category("State");
        r.field<&PlayerComponent::input_enabled>("input_enabled")
            .category("Input");
        r.field<&PlayerComponent::capsule_half_height>("capsule_half_height")
            .tooltip("Capsule half height").drag(0.01f).range(0.01f, 10.0f).category("Collision");
        r.field<&PlayerComponent::capsule_radius>("capsule_radius")
            .tooltip("Capsule radius").drag(0.01f).range(0.01f, 5.0f).category("Collision");
    }
};

struct FreecamComponent {
    float movement_speed = 5.0f;
    float fast_movement_speed = 15.0f;
    float mouse_sensitivity = 1.0f;
    bool input_enabled = true;

    static void reflect(Reflector<FreecamComponent>& r) {
        r.display("Freecam").category("Gameplay");
        r.field<&FreecamComponent::movement_speed>("movement_speed")
            .tooltip("Normal speed").drag(0.1f).range(0.0f, 100.0f).category("Movement");
        r.field<&FreecamComponent::fast_movement_speed>("fast_movement_speed")
            .tooltip("Fast speed (shift)").drag(0.1f).range(0.0f, 200.0f).category("Movement");
        r.field<&FreecamComponent::mouse_sensitivity>("mouse_sensitivity")
            .tooltip("Mouse sensitivity").drag(0.01f).range(0.01f, 10.0f).category("Input");
        r.field<&FreecamComponent::input_enabled>("input_enabled")
            .category("Input");
    }
};

struct PlayerRepresentationComponent {
    entt::entity tracked_player = entt::null;
    glm::vec3 position_offset;
    bool visible_only_freecam = true;

    static void reflect(Reflector<PlayerRepresentationComponent>& r) {
        r.display("Player Representation").category("Gameplay");
        r.field<&PlayerRepresentationComponent::tracked_player>("tracked_player")
            .tooltip("Entity to track").category("Tracking");
        r.field<&PlayerRepresentationComponent::position_offset>("position_offset")
            .tooltip("Offset from tracked entity").drag(0.01f).category("Tracking");
        r.field<&PlayerRepresentationComponent::visible_only_freecam>("visible_only_freecam")
            .tooltip("Only visible in freecam mode").category("Tracking");
    }
};

struct PointLightComponent {
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float constant_attenuation = 1.0f;
    float linear_attenuation = 0.09f;
    float quadratic_attenuation = 0.032f;

    static void reflect(Reflector<PointLightComponent>& r) {
        r.display("Point Light").category("Lighting");
        r.field<&PointLightComponent::color>("color")
            .tooltip("Light color").widget(EPropertyWidget::ColorEdit3).category("Light");
        r.field<&PointLightComponent::intensity>("intensity")
            .tooltip("Light intensity").drag(0.1f).range(0.0f, 100.0f).category("Light");
        r.field<&PointLightComponent::range>("range")
            .tooltip("Light range").drag(0.1f).range(0.1f, 1000.0f).category("Light");
        r.field<&PointLightComponent::constant_attenuation>("constant_attenuation")
            .tooltip("Constant").drag(0.01f).range(0.0f, 10.0f).category("Attenuation");
        r.field<&PointLightComponent::linear_attenuation>("linear_attenuation")
            .tooltip("Linear").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
        r.field<&PointLightComponent::quadratic_attenuation>("quadratic_attenuation")
            .tooltip("Quadratic").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
    }
};

struct SpotLightComponent {
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 15.0f;
    float inner_cone_angle = 12.5f;
    float outer_cone_angle = 17.5f;
    float constant_attenuation = 1.0f;
    float linear_attenuation = 0.09f;
    float quadratic_attenuation = 0.032f;

    static void reflect(Reflector<SpotLightComponent>& r) {
        r.display("Spot Light").category("Lighting");
        r.field<&SpotLightComponent::color>("color")
            .tooltip("Light color").widget(EPropertyWidget::ColorEdit3).category("Light");
        r.field<&SpotLightComponent::intensity>("intensity")
            .tooltip("Light intensity").drag(0.1f).range(0.0f, 100.0f).category("Light");
        r.field<&SpotLightComponent::range>("range")
            .tooltip("Light range").drag(0.1f).range(0.1f, 1000.0f).category("Light");
        r.field<&SpotLightComponent::inner_cone_angle>("inner_cone_angle")
            .tooltip("Inner cone angle (degrees)").drag(0.5f).range(0.0f, 90.0f).category("Light");
        r.field<&SpotLightComponent::outer_cone_angle>("outer_cone_angle")
            .tooltip("Outer cone angle (degrees)").drag(0.5f).range(0.0f, 90.0f).category("Light");
        r.field<&SpotLightComponent::constant_attenuation>("constant_attenuation")
            .tooltip("Constant").drag(0.01f).range(0.0f, 10.0f).category("Attenuation");
        r.field<&SpotLightComponent::linear_attenuation>("linear_attenuation")
            .tooltip("Linear").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
        r.field<&SpotLightComponent::quadratic_attenuation>("quadratic_attenuation")
            .tooltip("Quadratic").drag(0.001f).range(0.0f, 2.0f).category("Attenuation");
    }
};

// --- Constraint System ---

enum class ConstraintType : int
{
    Fixed = 0,
    Hinge,
    Point,
    Distance,
    COUNT
};

static const char* constraint_type_names[] = { "Fixed", "Hinge", "Point", "Distance" };

inline ConstraintType stringToConstraintType(const std::string& s)
{
    if (s == "Hinge")    return ConstraintType::Hinge;
    if (s == "Point")    return ConstraintType::Point;
    if (s == "Distance") return ConstraintType::Distance;
    return ConstraintType::Fixed;
}

inline std::string constraintTypeToString(ConstraintType t)
{
    switch (t)
    {
    case ConstraintType::Hinge:    return "Hinge";
    case ConstraintType::Point:    return "Point";
    case ConstraintType::Distance: return "Distance";
    default:                       return "Fixed";
    }
}

struct ConstraintComponent {
    ConstraintType type = ConstraintType::Fixed;
    std::string target_entity_name;
    entt::entity target_entity = entt::null;

    // Attachment points (local space of each body)
    glm::vec3 anchor_1 = glm::vec3(0.0f);
    glm::vec3 anchor_2 = glm::vec3(0.0f);

    // Hinge-specific
    glm::vec3 hinge_axis = glm::vec3(0.0f, 1.0f, 0.0f);
    float hinge_min_limit = -180.0f;
    float hinge_max_limit = 180.0f;

    // Distance-specific
    float min_distance = -1.0f;  // -1 = auto-detect from initial positions
    float max_distance = -1.0f;

    static void reflect(Reflector<ConstraintComponent>& r) {
        r.display("Constraint").category("Physics");
        r.field<&ConstraintComponent::type>("type")
            .tooltip("Constraint type")
            .enumValues(constraint_type_names, (int)ConstraintType::COUNT)
            .category("Constraint");
        r.field<&ConstraintComponent::target_entity_name>("target_entity_name")
            .tooltip("Name of target entity").category("Constraint");
        r.field<&ConstraintComponent::anchor_1>("anchor_1")
            .tooltip("Local anchor on this body").drag(0.01f).category("Constraint");
        r.field<&ConstraintComponent::anchor_2>("anchor_2")
            .tooltip("Local anchor on target body").drag(0.01f).category("Constraint");
        r.field<&ConstraintComponent::hinge_axis>("hinge_axis")
            .tooltip("Hinge rotation axis").drag(0.01f).category("Hinge");
        r.field<&ConstraintComponent::hinge_min_limit>("hinge_min_limit")
            .tooltip("Min angle (degrees)").drag(1.0f).range(-180.0f, 0.0f).category("Hinge");
        r.field<&ConstraintComponent::hinge_max_limit>("hinge_max_limit")
            .tooltip("Max angle (degrees)").drag(1.0f).range(0.0f, 180.0f).category("Hinge");
        r.field<&ConstraintComponent::min_distance>("min_distance")
            .tooltip("Min distance (-1 = auto)").drag(0.01f).category("Distance");
        r.field<&ConstraintComponent::max_distance>("max_distance")
            .tooltip("Max distance (-1 = auto)").drag(0.01f).category("Distance");
    }
};
