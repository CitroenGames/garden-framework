#include "PhysicsSystem.hpp"
#include "Assets/CookedCollisionSerializer.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <thread>

static void JoltTrace(const char* inFMT, ...)
{
    va_list args;
    va_start(args, inFMT);
    vprintf(inFMT, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static std::atomic<bool> s_jolt_registered{false};
static std::once_flag s_jolt_register_once;

static bool isFiniteVertexPosition(const vertex& v)
{
    return std::isfinite(v.vx) && std::isfinite(v.vy) && std::isfinite(v.vz);
}

static bool meshHasWaterMaterialRanges(const mesh& colliderMesh)
{
    if (!colliderMesh.uses_material_ranges)
        return false;

    return std::any_of(colliderMesh.material_ranges.begin(), colliderMesh.material_ranges.end(),
                       [](const MaterialRange& range) { return range.isWater(); });
}

static bool triangleUsesWaterMaterial(const mesh& colliderMesh, size_t triangle_start)
{
    if (!colliderMesh.uses_material_ranges)
        return false;

    for (const MaterialRange& range : colliderMesh.material_ranges)
    {
        if (!range.isWater())
            continue;

        const size_t range_end = range.start_vertex + range.vertex_count;
        if (triangle_start >= range.start_vertex && triangle_start + 2 < range_end)
            return true;
    }

    return false;
}

static uint32_t resolveWorkerThreadCount(uint32_t requested)
{
    if (requested > 0)
        return requested;

    const uint32_t hardware_threads = std::thread::hardware_concurrency();
    return hardware_threads > 1 ? hardware_threads - 1 : 1;
}

static PhysicsSystemSettings sanitizePhysicsSettings(PhysicsSystemSettings sanitized)
{
    sanitized.gravity_acceleration = std::max(sanitized.gravity_acceleration, 0.0f);
    sanitized.fixed_delta = std::max(sanitized.fixed_delta, 0.0001f);

    sanitized.max_bodies = std::max<uint32_t>(sanitized.max_bodies, 1);
    sanitized.max_body_pairs = std::max<uint32_t>(sanitized.max_body_pairs, 1);
    sanitized.max_contact_constraints = std::max<uint32_t>(sanitized.max_contact_constraints, 1);
    sanitized.temp_allocator_size_bytes = std::max<uint32_t>(sanitized.temp_allocator_size_bytes, 1024u * 1024u);
    sanitized.worker_thread_count = resolveWorkerThreadCount(sanitized.worker_thread_count);
    sanitized.collision_steps = std::max<uint32_t>(sanitized.collision_steps, 1);

    sanitized.max_body_velocity = std::max(sanitized.max_body_velocity, 0.0f);
    sanitized.max_character_velocity = std::max(sanitized.max_character_velocity, 0.0f);
    sanitized.min_body_mass = std::max(sanitized.min_body_mass, 0.000001f);
    sanitized.dynamic_linear_damping = std::max(sanitized.dynamic_linear_damping, 0.0f);
    sanitized.kinematic_gravity_factor = std::max(sanitized.kinematic_gravity_factor, 0.0f);
    sanitized.force_epsilon = std::max(sanitized.force_epsilon, 0.0f);

    sanitized.mesh_degenerate_triangle_epsilon = std::max(sanitized.mesh_degenerate_triangle_epsilon, 0.0f);
    sanitized.player_ground_probe_padding = std::max(sanitized.player_ground_probe_padding, 0.0f);
    sanitized.player_min_ground_normal_y = std::clamp(sanitized.player_min_ground_normal_y, -1.0f, 1.0f);
    sanitized.raycast_direction_epsilon = std::max(sanitized.raycast_direction_epsilon, 0.0f);

    if (sanitized.layers.object_layer_count <= sanitized.layers.static_body)
        sanitized.layers.object_layer_count = sanitized.layers.static_body + 1;
    if (sanitized.layers.object_layer_count <= sanitized.layers.dynamic_body)
        sanitized.layers.object_layer_count = sanitized.layers.dynamic_body + 1;
    if (sanitized.layers.object_layer_count <= sanitized.layers.character_body)
        sanitized.layers.object_layer_count = sanitized.layers.character_body + 1;
    if (sanitized.layers.object_layer_count <= sanitized.layers.trigger_body)
        sanitized.layers.object_layer_count = sanitized.layers.trigger_body + 1;
    sanitized.max_trigger_count = std::max<uint32_t>(sanitized.max_trigger_count, 1);

    const uint32_t static_broad_phase_index = uint32_t(sanitized.layers.static_broad_phase.GetValue());
    const uint32_t dynamic_broad_phase_index = uint32_t(sanitized.layers.dynamic_broad_phase.GetValue());
    const uint32_t needed_broad_phase_count = std::max(static_broad_phase_index, dynamic_broad_phase_index) + 1;
    sanitized.layers.broad_phase_layer_count = std::max(sanitized.layers.broad_phase_layer_count, needed_broad_phase_count);

    return sanitized;
}

static bool isUsableTriangle(const vertex& v0, const vertex& v1, const vertex& v2, float epsilon)
{
    if (!isFiniteVertexPosition(v0) || !isFiniteVertexPosition(v1) || !isFiniteVertexPosition(v2))
        return false;

    const glm::vec3 p0(v0.vx, v0.vy, v0.vz);
    const glm::vec3 p1(v1.vx, v1.vy, v1.vz);
    const glm::vec3 p2(v2.vx, v2.vy, v2.vz);
    const glm::vec3 area_vec = glm::cross(p1 - p0, p2 - p0);
    return glm::dot(area_vec, area_vec) > epsilon;
}

class IgnoreEntityBodyFilter final : public JPH::BodyFilter
{
public:
    IgnoreEntityBodyFilter(const std::unordered_map<JPH::BodyID, entt::entity>& body_to_entity,
                           entt::entity ignored_entity)
        : m_body_to_entity(body_to_entity), m_ignored_entity(ignored_entity)
    {
    }

    bool ShouldCollide(const JPH::BodyID& body_id) const override
    {
        if (m_ignored_entity == entt::null)
            return true;

        auto it = m_body_to_entity.find(body_id);
        return it == m_body_to_entity.end() || it->second != m_ignored_entity;
    }

    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return ShouldCollide(body.GetID());
    }

private:
    const std::unordered_map<JPH::BodyID, entt::entity>& m_body_to_entity;
    entt::entity m_ignored_entity = entt::null;
};

static uint32_t effectiveQueryLayerMask(const PhysicsLayerSettings& layers,
                                        const PhysicsSystem::QueryFilter& filter)
{
    uint32_t mask = filter.layer_mask;
    if (!filter.hit_triggers)
        mask &= ~PhysicsObjectLayers::layerBit(layers.trigger_body);
    return mask;
}

class QueryBroadPhaseLayerFilter final : public JPH::BroadPhaseLayerFilter
{
public:
    QueryBroadPhaseLayerFilter(const PhysicsLayerSettings& ls, uint32_t mask) : m_layers(ls), m_mask(mask) {}
    bool ShouldCollide(JPH::BroadPhaseLayer inLayer) const override
    {
        if (inLayer == m_layers.static_broad_phase)
            return (m_mask & PhysicsObjectLayers::layerBit(m_layers.static_body)) != 0;
        if (inLayer == m_layers.dynamic_broad_phase)
            return (m_mask & ~PhysicsObjectLayers::layerBit(m_layers.static_body)) != 0;
        return false;
    }
private:
    PhysicsLayerSettings m_layers; uint32_t m_mask;
};

class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter
{
public:
    explicit QueryObjectLayerFilter(uint32_t mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer inLayer) const override
    {
        return uint32_t(inLayer) < 32 && ((m_mask >> uint32_t(inLayer)) & 1u) != 0;
    }
private:
    uint32_t m_mask;
};

class QueryBodyFilter final : public JPH::BodyFilter
{
public:
    QueryBodyFilter(const std::unordered_map<JPH::BodyID, entt::entity>& map,
                    const PhysicsSystem::QueryFilter& filter, uint32_t mask)
        : m_map(map), m_filter(filter), m_mask(mask) {}
    bool ShouldCollide(const JPH::BodyID& body_id) const override
    {
        auto it = m_map.find(body_id);
        if (it == m_map.end()) return true;
        return !isIgnored(it->second);
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        if (uint32_t(body.GetObjectLayer()) >= 32 ||
            ((m_mask >> uint32_t(body.GetObjectLayer())) & 1u) == 0)
            return false;
        auto it = m_map.find(body.GetID());
        if (it == m_map.end()) return true;
        return !isIgnored(it->second);
    }
private:
    bool isIgnored(entt::entity e) const
    {
        if (e == m_filter.ignore) return true;
        if (m_filter.ignore_list)
            for (uint32_t i = 0; i < m_filter.ignore_count; ++i)
                if (m_filter.ignore_list[i] == e) return true;
        return false;
    }
    const std::unordered_map<JPH::BodyID, entt::entity>& m_map;
    PhysicsSystem::QueryFilter m_filter; uint32_t m_mask;
};

PhysicsSystem::PhysicsSystem(const PhysicsSystemSettings& system_settings)
{
    applySettings(system_settings);
}

PhysicsSystem::PhysicsSystem(const glm::vec3& gravityVector, float deltaTime)
{
    PhysicsSystemSettings defaults;
    defaults.gravity_direction = gravityVector;
    defaults.fixed_delta = deltaTime;
    applySettings(defaults);
}

PhysicsSystem::~PhysicsSystem()
{
    shutdown();
}

void PhysicsSystem::applySettings(const PhysicsSystemSettings& new_settings)
{
    settings = sanitizePhysicsSettings(new_settings);
    gravity = settings.gravity_direction;
    fixed_delta = settings.fixed_delta;

    broad_phase_layer_interface.setLayerSettings(settings.layers);
    object_vs_broadphase_filter.setLayerSettings(settings.layers);
    object_layer_pair_filter.setLayerSettings(settings.layers);
}

bool PhysicsSystem::configure(const PhysicsSystemSettings& new_settings)
{
    if (initialized)
    {
        LOG_ENGINE_WARN("PhysicsSystem::configure ignored after initialization; call shutdown before reconfiguring Jolt world settings");
        return false;
    }

    applySettings(new_settings);
    return true;
}

glm::vec3 PhysicsSystem::clampVelocity(const glm::vec3& v) const
{
    const float len = glm::length(v);
    if (settings.max_body_velocity > 0.0f && len > settings.max_body_velocity)
        return v * (settings.max_body_velocity / len);
    return v;
}

void PhysicsSystem::initialize()
{
    if (initialized) return;

    ensureJoltRegistered();

    // Create allocator and job system
    temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(settings.temp_allocator_size_bytes);
    job_system = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        settings.worker_thread_count);

    jolt_system = std::make_unique<JPH::PhysicsSystem>();
    jolt_system->Init(settings.max_bodies, settings.body_mutex_count, settings.max_body_pairs, settings.max_contact_constraints,
        broad_phase_layer_interface, object_vs_broadphase_filter, object_layer_pair_filter);

    jolt_system->SetGravity(toJolt(gravityAcceleration()));

    // Set up contact listener for collision events
    contact_listener = std::make_unique<EngineContactListener>();
    contact_listener->setBodyToEntityMap(&body_to_entity);
    jolt_system->SetContactListener(contact_listener.get());

    initialized = true;
    LOG_ENGINE_INFO("Jolt Physics initialized ({} worker threads, {} bodies)", settings.worker_thread_count, settings.max_bodies);
}

void PhysicsSystem::ensureJoltRegistered()
{
    // Register Jolt allocators and types (once globally). Asset import uses this
    // too when cooking .gardenphys files before any world has initialized.
    std::call_once(s_jolt_register_once, []() {
        // Set trace and assert handlers FIRST (before any Jolt code runs)
        JPH::Trace = JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = [](const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine) -> bool {
            printf("JOLT ASSERT FAILED: %s:%u: %s", inFile, inLine, inExpression);
            if (inMessage) printf(" (%s)", inMessage);
            printf("\n");
            fflush(stdout);
            return false; // don't trigger breakpoint
        };
#endif

        // Use Jolt's built-in default allocator (handles posix_memalign etc.)
        JPH::RegisterDefaultAllocator();

        if (!JPH::Factory::sInstance)
            JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        s_jolt_registered = true;
    });
}

void PhysicsSystem::shutdown()
{
    if (!initialized) return;

    // Remove all constraints
    if (jolt_system)
    {
        for (auto& [entity, constraint] : entity_to_constraint)
            jolt_system->RemoveConstraint(constraint);
    }
    entity_to_constraint.clear();

    character_controllers.shutdown(body_to_entity);

    // Remove all bodies
    if (jolt_system)
    {
        JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
        for (auto& [entity, body_id] : entity_to_body)
        {
            body_interface.RemoveBody(body_id);
            body_interface.DestroyBody(body_id);
        }
    }

    entity_to_body.clear();
    body_to_entity.clear();
    trigger_entities.clear();

    contact_listener.reset();
    jolt_system.reset();
    job_system.reset();
    temp_allocator.reset();

    // Note: Factory and registered types are intentionally NOT cleaned up here.
    // They are process-lifetime singletons that must outlive all PhysicsSystem instances.

    initialized = false;
}

void PhysicsSystem::setGravity(const glm::vec3& gravityVector)
{
    gravity = gravityVector;
    settings.gravity_direction = gravityVector;
    if (jolt_system)
    {
        jolt_system->SetGravity(toJolt(gravityAcceleration()));
    }
}

JPH::BodyInterface& PhysicsSystem::getBodyInterface()
{
    return jolt_system->GetBodyInterface();
}

void PhysicsSystem::optimizeBroadPhase()
{
    if (jolt_system)
        jolt_system->OptimizeBroadPhase();
}

JPH::BodyID PhysicsSystem::createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
    const PhysicsBodyDesc& desc)
{
    if (!initialized) initialize();
    if (!shape) return JPH::BodyID();
    if (entity_to_body.find(entity) != entity_to_body.end())
        removeBody(entity);

    JPH::BodyCreationSettings body_settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Static, settings.layers.static_body);
    body_settings.mFriction = desc.friction;
    body_settings.mRestitution = desc.restitution;

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(body_settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::DontActivate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    return body_id;
}

JPH::BodyID PhysicsSystem::createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    return createStaticBody(position, rotation, shape, entity, PhysicsBodyDesc{});
}

JPH::ShapeRefC PhysicsSystem::createShapeFromCollider(const ColliderComponent& collider, const glm::vec3& scale)
{
    JPH::ShapeRefC shape;

    switch (collider.shape_type)
    {
    case ColliderShapeType::Box: {
        JPH::BoxShapeSettings settings(toJolt(collider.box_half_extents));
        auto result = settings.Create();
        if (result.IsValid()) shape = result.Get();
        break;
    }
    case ColliderShapeType::Sphere: {
        JPH::SphereShapeSettings settings(collider.sphere_radius);
        auto result = settings.Create();
        if (result.IsValid()) shape = result.Get();
        break;
    }
    case ColliderShapeType::Capsule: {
        JPH::CapsuleShapeSettings settings(collider.capsule_half_height, collider.capsule_radius);
        auto result = settings.Create();
        if (result.IsValid()) shape = result.Get();
        break;
    }
    case ColliderShapeType::Cylinder: {
        JPH::CylinderShapeSettings settings(collider.cylinder_half_height, collider.cylinder_radius);
        auto result = settings.Create();
        if (result.IsValid()) shape = result.Get();
        break;
    }
    case ColliderShapeType::ConvexHull: {
        if (collider.m_mesh && collider.m_mesh->is_valid && collider.m_mesh->vertices_len > 0) {
            JPH::Array<JPH::Vec3> points;
            points.reserve(collider.m_mesh->vertices_len);
            for (size_t i = 0; i < collider.m_mesh->vertices_len; i++) {
                const auto& v = collider.m_mesh->vertices[i];
                points.push_back(JPH::Vec3(v.vx, v.vy, v.vz));
            }
            JPH::ConvexHullShapeSettings settings(points.data(), (int)points.size());
            auto result = settings.Create();
            if (result.IsValid()) shape = result.Get();
        }
        break;
    }
    case ColliderShapeType::Mesh:
    default:
        // Mesh shapes are handled by createStaticMeshBody path
        return nullptr;
    }

    // Apply scale via ScaledShape if non-identity
    if (shape && (scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f))
    {
        JPH::ScaledShapeSettings scaled(shape, toJolt(scale));
        auto result = scaled.Create();
        if (result.IsValid()) return result.Get();
    }

    return shape;
}

JPH::BodyID PhysicsSystem::createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
    const PhysicsBodyDesc& desc)
{
    if (!initialized) initialize();
    if (!shape) return JPH::BodyID();
    if (entity_to_body.find(entity) != entity_to_body.end())
        removeBody(entity);

    JPH::BodyCreationSettings body_settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Dynamic, settings.layers.dynamic_body);
    body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    body_settings.mMassPropertiesOverride.mMass = std::max(desc.mass, settings.min_body_mass);
    if (desc.lock_rotation)
        body_settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
    body_settings.mMotionQuality = JPH::EMotionQuality::LinearCast; // CCD to prevent tunneling
    body_settings.mFriction = desc.friction;
    body_settings.mRestitution = desc.restitution;
    body_settings.mGravityFactor = desc.apply_gravity ? 1.0f : 0.0f;
    body_settings.mLinearDamping = settings.dynamic_linear_damping;

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(body_settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::Activate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    LOG_ENGINE_INFO("Created Jolt dynamic body at ({}, {}, {}), mass={}", position.x, position.y, position.z, desc.mass);
    return body_id;
}

JPH::BodyID PhysicsSystem::createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    return createDynamicBody(position, rotation, shape, entity, PhysicsBodyDesc{});
}

JPH::BodyID PhysicsSystem::createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity,
    float friction, float restitution, bool lock_rotation)
{
    PhysicsBodyDesc desc;
    desc.mass = mass;
    desc.friction = friction;
    desc.restitution = restitution;
    desc.lock_rotation = lock_rotation;
    return createDynamicBody(position, rotation, shape, entity, desc);
}

JPH::BodyID PhysicsSystem::createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
    const PhysicsBodyDesc& desc)
{
    if (!initialized) initialize();
    if (!shape) return JPH::BodyID();
    if (entity_to_body.find(entity) != entity_to_body.end())
        removeBody(entity);

    JPH::BodyCreationSettings body_settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Kinematic, settings.layers.dynamic_body);
    body_settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    body_settings.mFriction = desc.friction;
    body_settings.mRestitution = desc.restitution;
    body_settings.mGravityFactor = settings.kinematic_gravity_factor;

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(body_settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::Activate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    LOG_ENGINE_INFO("Created Jolt kinematic body at ({}, {}, {})", position.x, position.y, position.z);
    return body_id;
}

JPH::BodyID PhysicsSystem::createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    return createKinematicBody(position, rotation, shape, entity, PhysicsBodyDesc{});
}

JPH::BodyID PhysicsSystem::createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity,
    const PhysicsBodyDesc& desc)
{
    if (!initialized) initialize();

    JPH::ShapeRefC final_shape;
    const bool exclude_water_collision = meshHasWaterMaterialRanges(colliderMesh);
    if (!exclude_water_collision && !colliderMesh.collision_cache_path.empty())
    {
        std::string error;
        if (Assets::CookedCollisionSerializer::loadShape(
                colliderMesh.collision_cache_path,
                colliderMesh.collision_source_hash,
                colliderMesh.collision_source_file_size,
                final_shape,
                &error))
        {
            LOG_ENGINE_TRACE("Loaded cooked collision mesh: {}", colliderMesh.collision_cache_path);
        }
        else
        {
            LOG_ENGINE_WARN("Failed to load cooked collision '{}': {}",
                            colliderMesh.collision_cache_path, error);
        }
    }
    else if (exclude_water_collision && !colliderMesh.collision_cache_path.empty())
    {
        LOG_ENGINE_TRACE("Ignoring cooked collision mesh '{}' because water material ranges are non-solid",
                         colliderMesh.collision_cache_path);
    }

    if (!final_shape)
    {
        if (!colliderMesh.vertices || colliderMesh.vertices_len < 3) return JPH::BodyID();

        // Build Jolt triangle list from mesh vertices (in local space)
        JPH::TriangleList triangles;
        triangles.reserve(colliderMesh.vertices_len / 3);

        for (size_t i = 0; i + 2 < colliderMesh.vertices_len; i += 3)
        {
            if (triangleUsesWaterMaterial(colliderMesh, i))
                continue;

            const vertex& v0 = colliderMesh.vertices[i];
            const vertex& v1 = colliderMesh.vertices[i + 1];
            const vertex& v2 = colliderMesh.vertices[i + 2];
            if (!isUsableTriangle(v0, v1, v2, settings.mesh_degenerate_triangle_epsilon))
                continue;

            triangles.push_back(JPH::Triangle(
                JPH::Float3(v0.vx, v0.vy, v0.vz),
                JPH::Float3(v1.vx, v1.vy, v1.vz),
                JPH::Float3(v2.vx, v2.vy, v2.vz)
            ));
        }
        if (triangles.empty())
        {
            LOG_ENGINE_ERROR("Failed to create Jolt mesh shape: no usable triangles");
            return JPH::BodyID();
        }

        LOG_ENGINE_INFO("Creating Jolt mesh body: {} triangles at ({}, {}, {}), scale=({}, {}, {})",
            triangles.size(), position.x, position.y, position.z, scale.x, scale.y, scale.z);

        JPH::MeshShapeSettings mesh_settings(triangles);
        JPH::ShapeSettings::ShapeResult result = mesh_settings.Create();
        if (!result.IsValid())
        {
            LOG_ENGINE_ERROR("Failed to create Jolt mesh shape: {}", result.GetError().c_str());
            return JPH::BodyID();
        }
        final_shape = result.Get();
    }

    // Apply scale via ScaledShape if not identity
    if (scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f)
    {
        JPH::ScaledShapeSettings scaled_settings(final_shape, toJolt(scale));
        JPH::ShapeSettings::ShapeResult scaled_result = scaled_settings.Create();
        if (!scaled_result.IsValid())
        {
            LOG_ENGINE_ERROR("Failed to create scaled mesh shape: {}", scaled_result.GetError().c_str());
            return JPH::BodyID();
        }
        final_shape = scaled_result.Get();
    }

    return createStaticBody(position, rotation, final_shape, entity, desc);
}

JPH::BodyID PhysicsSystem::createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity)
{
    return createStaticMeshBody(position, rotation, scale, colliderMesh, entity, PhysicsBodyDesc{});
}

JPH::BodyID PhysicsSystem::createPlayerBody(entt::registry& registry, entt::entity entity)
{
    if (!registry.valid(entity))
        return JPH::BodyID();
    if (!registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(entity))
        return JPH::BodyID();

    character_controllers.copyPlayerSettings(registry, entity);
    return createCharacterController(registry, entity);
}

JPH::BodyID PhysicsSystem::createCharacterController(entt::registry& registry, entt::entity entity)
{
    if (!registry.valid(entity))
        return JPH::BodyID();
    if (!registry.all_of<TransformComponent, CharacterControllerComponent>(entity))
        return JPH::BodyID();

    if (!initialized)
        initialize();

    return character_controllers.create(
        registry, entity, *jolt_system, *temp_allocator, body_to_entity, settings.layers);
}

void PhysicsSystem::removeCharacterController(entt::entity entity)
{
    character_controllers.remove(entity, body_to_entity);
}

CharacterControllerState PhysicsSystem::getCharacterControllerState(entt::registry& registry, entt::entity entity) const
{
    return character_controllers.getState(registry, entity);
}

bool PhysicsSystem::setCharacterControllerState(entt::registry& registry, entt::entity entity,
    const CharacterControllerState& state)
{
    if (!initialized)
        initialize();

    return character_controllers.setState(
        registry, entity, state, *jolt_system, *temp_allocator, settings.layers);
}

bool PhysicsSystem::teleportCharacterController(entt::registry& registry, entt::entity entity, const glm::vec3& position)
{
    if (!initialized)
        initialize();

    return character_controllers.teleport(
        registry, entity, position, *jolt_system, *temp_allocator, settings.layers);
}

bool PhysicsSystem::teleportBody(entt::registry& registry, entt::entity entity,
    const glm::vec3& position, const glm::vec3& euler_degrees)
{
    if (!registry.valid(entity) || !isValidVec3(position) || !isValidVec3(euler_degrees) || stepping ||
        !registry.all_of<TransformComponent>(entity))
        return false;

    if (registry.all_of<CharacterControllerComponent>(entity))
    {
        if (!hasCharacterController(entity) || !teleportCharacterController(registry, entity, position))
            return false;

        auto& transform = registry.get<TransformComponent>(entity);
        transform.rotation = euler_degrees;
        if (registry.all_of<RigidBodyComponent>(entity))
            registry.get<RigidBodyComponent>(entity).force = glm::vec3(0.0f);
        return true;
    }

    const auto body_it = entity_to_body.find(entity);
    if (body_it == entity_to_body.end())
        return false;
    if (!initialized)
        initialize();

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    const JPH::EMotionType motion_type = body_interface.GetMotionType(body_it->second);
    if (motion_type != JPH::EMotionType::Dynamic && motion_type != JPH::EMotionType::Kinematic)
        return false;

    body_interface.SetPositionRotationAndVelocity(
        body_it->second, toJoltR(position), toJoltQuat(euler_degrees),
        JPH::Vec3::sZero(), JPH::Vec3::sZero());
    body_interface.ActivateBody(body_it->second);

    auto& transform = registry.get<TransformComponent>(entity);
    transform.position = position;
    transform.rotation = euler_degrees;
    if (registry.all_of<RigidBodyComponent>(entity))
    {
        auto& rigid_body = registry.get<RigidBodyComponent>(entity);
        rigid_body.velocity = glm::vec3(0.0f);
        rigid_body.force = glm::vec3(0.0f);
    }
    return true;
}

CharacterControllerState PhysicsSystem::simulateCharacterController(entt::registry& registry, entt::entity entity,
    const CharacterMoveInput& input, float delta_time)
{
    if (!initialized)
        initialize();

    return character_controllers.simulate(
        registry, entity, input, delta_time, settings,
        *jolt_system, *temp_allocator, body_to_entity);
}

PhysicsSystem::ShapeCastResult PhysicsSystem::shapeCast(const JPH::ShapeRefC& shape, const glm::vec3& position,
    const glm::vec3& rotation, const glm::vec3& direction)
{
    ShapeCastResult result;
    if (!initialized || !shape) return result;

    JPH::RMat44 com_start = JPH::RMat44::sRotationTranslation(
        toJoltQuat(rotation), toJoltR(position));

    JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(
        shape, JPH::Vec3::sReplicate(1.0f), com_start, toJolt(direction));

    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;

    jolt_system->GetNarrowPhaseQuery().CastShape(
        shape_cast, settings, com_start.GetTranslation(), collector);

    if (collector.HadHit())
    {
        result.hit = true;
        result.fraction = collector.mHit.mFraction;

        // Compute contact point along the cast direction
        JPH::RVec3 hit_point = com_start.GetTranslation() + JPH::Vec3(toJolt(direction)) * collector.mHit.mFraction;
        result.contact_point = glm::vec3(float(hit_point.GetX()), float(hit_point.GetY()), float(hit_point.GetZ()));

        if (collector.mHit.mPenetrationAxis.LengthSq() > 0.0f)
            result.contact_normal = toGlm(collector.mHit.mPenetrationAxis.Normalized());

        auto it = body_to_entity.find(collector.mHit.mBodyID2);
        if (it != body_to_entity.end())
            result.entity = it->second;
    }

    return result;
}

PhysicsSystem::ShapeCastResult PhysicsSystem::sphereCast(const glm::vec3& origin, float radius,
    const glm::vec3& direction, float maxDistance)
{
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon)
        return {};

    JPH::SphereShapeSettings sphere(radius);
    auto shape_result = sphere.Create();
    if (!shape_result.IsValid()) return {};

    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    return shapeCast(shape_result.Get(), origin, glm::vec3(0), dir);
}

PhysicsSystem::ShapeCastResult PhysicsSystem::boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
    const glm::vec3& rotation, const glm::vec3& direction, float maxDistance)
{
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon)
        return {};

    JPH::BoxShapeSettings box(toJolt(halfExtents));
    auto shape_result = box.Create();
    if (!shape_result.IsValid()) return {};

    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    return shapeCast(shape_result.Get(), origin, rotation, dir);
}

JPH::Constraint* PhysicsSystem::createConstraint(entt::entity entityA, entt::entity entityB, const ConstraintComponent& constraint)
{
    if (!initialized) return nullptr;

    auto itA = entity_to_body.find(entityA);
    auto itB = entity_to_body.find(entityB);
    if (itA == entity_to_body.end() || itB == entity_to_body.end()) return nullptr;

    JPH::BodyLockWrite lockA(jolt_system->GetBodyLockInterface(), itA->second);
    JPH::BodyLockWrite lockB(jolt_system->GetBodyLockInterface(), itB->second);
    if (!lockA.Succeeded() || !lockB.Succeeded()) return nullptr;

    JPH::TwoBodyConstraint* jolt_constraint = nullptr;

    switch (constraint.type)
    {
    case ConstraintType::Fixed: {
        JPH::FixedConstraintSettings settings;
        settings.mAutoDetectPoint = true;
        jolt_constraint = settings.Create(lockA.GetBody(), lockB.GetBody());
        break;
    }
    case ConstraintType::Hinge: {
        JPH::HingeConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = toJolt(constraint.anchor_1);
        settings.mPoint2 = toJolt(constraint.anchor_2);
        settings.mHingeAxis1 = toJolt(glm::normalize(constraint.hinge_axis));
        settings.mHingeAxis2 = toJolt(glm::normalize(constraint.hinge_axis));
        settings.mNormalAxis1 = JPH::Vec3::sAxisX();
        settings.mNormalAxis2 = JPH::Vec3::sAxisX();
        settings.mLimitsMin = glm::radians(constraint.hinge_min_limit);
        settings.mLimitsMax = glm::radians(constraint.hinge_max_limit);
        jolt_constraint = settings.Create(lockA.GetBody(), lockB.GetBody());
        break;
    }
    case ConstraintType::Point: {
        JPH::PointConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = toJolt(constraint.anchor_1);
        settings.mPoint2 = toJolt(constraint.anchor_2);
        jolt_constraint = settings.Create(lockA.GetBody(), lockB.GetBody());
        break;
    }
    case ConstraintType::Distance: {
        JPH::DistanceConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = toJolt(constraint.anchor_1);
        settings.mPoint2 = toJolt(constraint.anchor_2);
        if (constraint.min_distance >= 0.0f) settings.mMinDistance = constraint.min_distance;
        if (constraint.max_distance >= 0.0f) settings.mMaxDistance = constraint.max_distance;
        jolt_constraint = settings.Create(lockA.GetBody(), lockB.GetBody());
        break;
    }
    }

    if (jolt_constraint)
    {
        jolt_system->AddConstraint(jolt_constraint);
        entity_to_constraint[entityA] = jolt_constraint;
        LOG_ENGINE_INFO("Created {} constraint between entities", constraintTypeToString(constraint.type));
    }

    return jolt_constraint;
}

void PhysicsSystem::removeConstraint(entt::entity entity)
{
    auto it = entity_to_constraint.find(entity);
    if (it == entity_to_constraint.end()) return;

    if (jolt_system)
        jolt_system->RemoveConstraint(it->second);

    entity_to_constraint.erase(it);
}

void PhysicsSystem::removeBody(entt::entity entity)
{
    trigger_entities.erase(std::remove(trigger_entities.begin(), trigger_entities.end(), entity), trigger_entities.end());
    removeCharacterController(entity);

    // Remove any constraint associated with this entity first
    removeConstraint(entity);

    auto it = entity_to_body.find(entity);
    if (it == entity_to_body.end()) return;

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    body_interface.RemoveBody(it->second);
    body_interface.DestroyBody(it->second);

    body_to_entity.erase(it->second);
    entity_to_body.erase(it);
}

void PhysicsSystem::stepPhysics(entt::registry& registry)
{
    if (!initialized) return;
    stepping = true;

    // Sync ECS -> Jolt for dynamic bodies (in case game code moved them)
    syncTransformsToJolt(registry);

    // Step Jolt physics
    jolt_system->Update(fixed_delta, settings.collision_steps, temp_allocator.get(), job_system.get());

    // Drain collision events to EventBus (main thread)
    if (contact_listener)
        contact_listener->drainEvents();

    // Sync Jolt -> ECS for bodies managed by Jolt
    syncTransformsFromJolt(registry);

    // Fallback: integrate entities that have RigidBody but no Jolt body
    // (e.g. player controlled by PlayerController)
    auto view = registry.view<RigidBodyComponent, TransformComponent>();
    for (auto entity : view)
    {
        if (entity_to_body.find(entity) != entity_to_body.end())
            continue; // Managed by Jolt, already synced
        if (character_controllers.has(entity))
            continue; // Character controllers are advanced explicitly by game/network code

        auto& rb = view.get<RigidBodyComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);

        if (rb.apply_gravity)
            rb.velocity += gravityAcceleration() * fixed_delta;

        rb.velocity += rb.force * fixed_delta;
        transform.position += rb.velocity * fixed_delta;
        rb.force = glm::vec3(0);
    }
    stepping = false;
    // Polling trigger evaluation: post-step positions, queued events (D6).
    updateTriggers(registry);
}

void PhysicsSystem::syncTransformsFromJolt(entt::registry& registry)
{
    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();

    for (auto& [entity, body_id] : entity_to_body)
    {
        if (!registry.valid(entity)) continue;
        if (!registry.all_of<TransformComponent, RigidBodyComponent>(entity)) continue;

        // Only sync dynamic bodies
        if (body_interface.GetMotionType(body_id) != JPH::EMotionType::Dynamic) continue;

        auto& transform = registry.get<TransformComponent>(entity);
        auto& rb = registry.get<RigidBodyComponent>(entity);

        JPH::RVec3 pos = body_interface.GetCenterOfMassPosition(body_id);
        JPH::Vec3 vel = body_interface.GetLinearVelocity(body_id);

        glm::vec3 new_pos = glm::vec3(float(pos.GetX()), float(pos.GetY()), float(pos.GetZ()));
        if (!isValidVec3(new_pos))
        {
            LOG_ENGINE_ERROR("NaN position read from Jolt, keeping previous position");
            continue;
        }
        transform.position = new_pos;

        // Only read velocity for non-player entities — game code is velocity authority for the player
        if (!registry.all_of<PlayerComponent>(entity))
        {
            rb.velocity = toGlm(vel);
        }

    }
}

void PhysicsSystem::syncTransformsToJolt(entt::registry& registry)
{
    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();

    for (auto& [entity, body_id] : entity_to_body)
    {
        if (!registry.valid(entity)) continue;
        if (!registry.all_of<TransformComponent, RigidBodyComponent>(entity)) continue;

        auto motion_type = body_interface.GetMotionType(body_id);

        // Kinematic bodies: game code moves them via position, use MoveKinematic
        if (motion_type == JPH::EMotionType::Kinematic)
        {
            auto& transform = registry.get<TransformComponent>(entity);
            body_interface.MoveKinematic(body_id, toJoltR(transform.position), toJoltQuat(transform.rotation), fixed_delta);
            continue;
        }

        if (motion_type != JPH::EMotionType::Dynamic) continue;

        auto& rb = registry.get<RigidBodyComponent>(entity);

        // Validate and clamp velocity before pushing to Jolt
        if (!isValidVec3(rb.velocity))
        {
            LOG_ENGINE_WARN("NaN velocity detected, resetting to zero");
            rb.velocity = glm::vec3(0);
        }
        rb.velocity = clampVelocity(rb.velocity);
        body_interface.SetLinearVelocity(body_id, toJolt(rb.velocity));

        // If game code applied forces, send them to Jolt (force = acceleration * mass)
        if (glm::length(rb.force) > settings.force_epsilon)
        {
            if (isValidVec3(rb.force))
            {
                body_interface.AddForce(body_id, toJolt(rb.force * rb.mass));
            }
            rb.force = glm::vec3(0);
        }
    }
}

void PhysicsSystem::handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity)
{
    if (!initialized || !registry.valid(playerEntity)) return;
    if (registry.all_of<CharacterControllerComponent>(playerEntity) && hasCharacterController(playerEntity))
    {
        character_controllers.refresh(registry, playerEntity, *jolt_system, *temp_allocator, settings.layers);
        return;
    }
    if (!registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(playerEntity)) return;

    auto& player = registry.get<PlayerComponent>(playerEntity);

    // Check if player has a Jolt body
    auto it = entity_to_body.find(playerEntity);

    if (it != entity_to_body.end())
    {
        // Ground detection: cast a ray downward, excluding the player's own body
        auto& transform = registry.get<TransformComponent>(playerEntity);
        // Ray length derived from capsule dimensions + tolerance
        float ground_ray_length = player.capsule_half_height + player.capsule_radius + settings.player_ground_probe_padding;
        JPH::RRayCast ray(toJoltR(transform.position), JPH::Vec3(0, -1, 0) * ground_ray_length);
        JPH::RayCastResult hit;
        JPH::IgnoreSingleBodyFilter body_filter(it->second);

        player.grounded = jolt_system->GetNarrowPhaseQuery().CastRay(
            ray, hit,
            JPH::BroadPhaseLayerFilter(),
            JPH::ObjectLayerFilter(),
            body_filter);
        if (player.grounded)
        {
            // Get actual surface normal from hit result
            JPH::RVec3 hitPos = ray.GetPointOnRay(hit.mFraction);
            JPH::BodyLockRead lock(jolt_system->GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded())
            {
                JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
                player.ground_normal = toGlm(normal);
            }
            else
            {
                player.ground_normal = glm::vec3(0, 1, 0);
            }

            // Reject surfaces too steep to stand on (~45 degrees)
            if (player.ground_normal.y < settings.player_min_ground_normal_y)
            {
                player.grounded = false;
                player.ground_normal = glm::vec3(0, 1, 0);
            }
        }

        LOG_ENGINE_TRACE("Player collision: grounded={}, pos=({},{},{})",
            player.grounded, transform.position.x, transform.position.y, transform.position.z);
    }
    else
    {
        // Fallback: no Jolt body, just reset ground state
        player.grounded = false;
        player.ground_normal = glm::vec3(0, 1, 0);
        LOG_ENGINE_WARN("Player collision: has_body=FALSE — player has no Jolt body, using fallback (no collision!)");
    }
}


// ---- D6: QueryFilter queries, overlaps, triggers ----

PhysicsSystem::RaycastResult PhysicsSystem::raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
    float maxDistance, const QueryFilter& filter)
{
    RaycastResult result;
    assert(!stepping && "physics queries are prohibited during stepPhysics");
    if (!initialized || stepping) return result;
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon) return result;
    const uint32_t mask = effectiveQueryLayerMask(settings.layers, filter);
    JPH::Vec3 dir = toJolt(glm::normalize(direction)) * maxDistance;
    JPH::RRayCast ray(toJoltR(origin), dir);
    JPH::RayCastResult hit;
    QueryBroadPhaseLayerFilter bp_filter(settings.layers, mask);
    QueryObjectLayerFilter obj_filter(mask);
    QueryBodyFilter body_filter(body_to_entity, filter, mask);
    if (jolt_system->GetNarrowPhaseQuery().CastRay(ray, hit, bp_filter, obj_filter, body_filter))
    {
        JPH::RVec3 hitPos = ray.GetPointOnRay(hit.mFraction);
        result.hit = true;
        result.hit_point = glm::vec3(float(hitPos.GetX()), float(hitPos.GetY()), float(hitPos.GetZ()));
        result.fraction = hit.mFraction;
        result.distance = hit.mFraction * maxDistance;
        JPH::BodyLockRead lock(jolt_system->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            result.hit_normal = toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos));
        else
            result.hit_normal = glm::vec3(0, 1, 0);
        auto it = body_to_entity.find(hit.mBodyID);
        if (it != body_to_entity.end()) result.entity = it->second;
    }
    return result;
}

uint32_t PhysicsSystem::raycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    const QueryFilter& filter, RaycastResult* out, uint32_t cap)
{
    assert(!stepping && "physics queries are prohibited during stepPhysics");
    if (!initialized || stepping || !out || cap == 0) return 0;
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon) return 0;
    const uint32_t mask = effectiveQueryLayerMask(settings.layers, filter);
    JPH::Vec3 dir = toJolt(glm::normalize(direction)) * maxDistance;
    JPH::RRayCast ray(toJoltR(origin), dir);
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    JPH::RayCastSettings rc_settings;
    QueryBroadPhaseLayerFilter bp_filter(settings.layers, mask);
    QueryObjectLayerFilter obj_filter(mask);
    QueryBodyFilter body_filter(body_to_entity, filter, mask);
    JPH::ShapeFilter shape_filter;
    jolt_system->GetNarrowPhaseQuery().CastRay(ray, rc_settings, collector, bp_filter, obj_filter, body_filter, shape_filter);
    collector.Sort();
    uint32_t count = 0;
    for (const auto& hit : collector.mHits)
    {
        if (count >= cap) break;
        RaycastResult& r = out[count];
        JPH::RVec3 hitPos = ray.GetPointOnRay(hit.mFraction);
        r.hit = true;
        r.hit_point = glm::vec3(float(hitPos.GetX()), float(hitPos.GetY()), float(hitPos.GetZ()));
        r.fraction = hit.mFraction;
        r.distance = hit.mFraction * maxDistance;
        JPH::BodyLockRead lock(jolt_system->GetBodyLockInterface(), hit.mBodyID);
        r.hit_normal = lock.Succeeded()
            ? toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos))
            : glm::vec3(0, 1, 0);
        auto it = body_to_entity.find(hit.mBodyID);
        r.entity = (it != body_to_entity.end()) ? it->second : entt::null;
        ++count;
    }
    return count;
}

uint32_t PhysicsSystem::overlapShape(const JPH::ShapeRefC& shape, const glm::vec3& center, const glm::vec3& rotation,
    const QueryFilter& filter, OverlapResult* out, uint32_t cap)
{
    assert(!stepping && "physics queries are prohibited during stepPhysics");
    if (!initialized || stepping || !shape || !out || cap == 0) return 0;
    const uint32_t mask = effectiveQueryLayerMask(settings.layers, filter);
    JPH::RMat44 com = JPH::RMat44::sRotationTranslation(toJoltQuat(rotation), toJoltR(center));
    JPH::CollideShapeSettings cs;
    cs.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
    cs.mMaxSeparationDistance = 0.0f;
    cs.mCollectFacesMode = JPH::ECollectFacesMode::NoFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryBroadPhaseLayerFilter bp_filter(settings.layers, mask);
    QueryObjectLayerFilter obj_filter(mask);
    QueryBodyFilter body_filter(body_to_entity, filter, mask);
    JPH::ShapeFilter shape_filter;
    jolt_system->GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1.0f), com, cs,
        JPH::RVec3::sZero(), collector, bp_filter, obj_filter, body_filter, shape_filter);
    std::vector<JPH::BodyID> seen;
    uint32_t count = 0;
    for (const auto& hit : collector.mHits)
    {
        if (std::find(seen.begin(), seen.end(), hit.mBodyID2) != seen.end()) continue;
        seen.push_back(hit.mBodyID2);
        auto it = body_to_entity.find(hit.mBodyID2);
        if (it == body_to_entity.end()) continue;
        if (count >= cap) break;
        out[count].entity = it->second;
        out[count].point = toGlm(hit.mContactPointOn2);
        ++count;
    }
    std::sort(out, out + count, [](const OverlapResult& a, const OverlapResult& b) {
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    });
    return count;
}

uint32_t PhysicsSystem::overlapSphere(const glm::vec3& center, float radius,
    const QueryFilter& filter, OverlapResult* out, uint32_t cap)
{
    if (radius <= 0.0f) return 0;
    JPH::SphereShapeSettings s(radius);
    auto r = s.Create();
    if (!r.IsValid()) return 0;
    return overlapShape(r.Get(), center, glm::vec3(0), filter, out, cap);
}

uint32_t PhysicsSystem::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& rotation,
    const QueryFilter& filter, OverlapResult* out, uint32_t cap)
{
    if (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f) return 0;
    JPH::BoxShapeSettings s(toJolt(halfExtents));
    auto r = s.Create();
    if (!r.IsValid()) return 0;
    return overlapShape(r.Get(), center, rotation, filter, out, cap);
}

uint32_t PhysicsSystem::overlapCapsule(const glm::vec3& center, float halfHeight, float radius,
    const QueryFilter& filter, OverlapResult* out, uint32_t cap)
{
    if (radius <= 0.0f || halfHeight < 0.0f) return 0;
    JPH::CapsuleShapeSettings s(halfHeight, radius);
    auto r = s.Create();
    if (!r.IsValid()) return 0;
    return overlapShape(r.Get(), center, glm::vec3(0), filter, out, cap);
}

PhysicsSystem::ShapeCastResult PhysicsSystem::shapeCast(const JPH::ShapeRefC& shape, const glm::vec3& position,
    const glm::vec3& rotation, const glm::vec3& direction, const QueryFilter& filter)
{
    ShapeCastResult result;
    assert(!stepping && "physics queries are prohibited during stepPhysics");
    if (!initialized || stepping || !shape) return result;
    const uint32_t mask = effectiveQueryLayerMask(settings.layers, filter);
    JPH::RMat44 com_start = JPH::RMat44::sRotationTranslation(toJoltQuat(rotation), toJoltR(position));
    JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(shape, JPH::Vec3::sReplicate(1.0f), com_start, toJolt(direction));
    JPH::ShapeCastSettings sc;
    sc.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    sc.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryBroadPhaseLayerFilter bp_filter(settings.layers, mask);
    QueryObjectLayerFilter obj_filter(mask);
    QueryBodyFilter body_filter(body_to_entity, filter, mask);
    JPH::ShapeFilter shape_filter;
    jolt_system->GetNarrowPhaseQuery().CastShape(shape_cast, sc, com_start.GetTranslation(), collector,
        bp_filter, obj_filter, body_filter, shape_filter);
    if (collector.HadHit())
    {
        result.hit = true;
        result.fraction = collector.mHit.mFraction;
        JPH::RVec3 hit_point = com_start.GetTranslation() + JPH::Vec3(toJolt(direction)) * collector.mHit.mFraction;
        result.contact_point = glm::vec3(float(hit_point.GetX()), float(hit_point.GetY()), float(hit_point.GetZ()));
        if (collector.mHit.mPenetrationAxis.LengthSq() > 0.0f)
            result.contact_normal = toGlm(collector.mHit.mPenetrationAxis.Normalized());
        auto it = body_to_entity.find(collector.mHit.mBodyID2);
        if (it != body_to_entity.end()) result.entity = it->second;
    }
    return result;
}

PhysicsSystem::ShapeCastResult PhysicsSystem::sphereCast(const glm::vec3& origin, float radius,
    const glm::vec3& direction, float maxDistance, const QueryFilter& filter)
{
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon) return {};
    JPH::SphereShapeSettings sphere(radius);
    auto shape_result = sphere.Create();
    if (!shape_result.IsValid()) return {};
    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    return shapeCast(shape_result.Get(), origin, glm::vec3(0), dir, filter);
}

PhysicsSystem::ShapeCastResult PhysicsSystem::boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
    const glm::vec3& rotation, const glm::vec3& direction, float maxDistance, const QueryFilter& filter)
{
    if (maxDistance <= 0.0f || glm::length(direction) <= settings.raycast_direction_epsilon) return {};
    JPH::BoxShapeSettings box(toJolt(halfExtents));
    auto shape_result = box.Create();
    if (!shape_result.IsValid()) return {};
    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    return shapeCast(shape_result.Get(), origin, rotation, dir, filter);
}

JPH::BodyID PhysicsSystem::createTriggerBody(entt::registry& registry, entt::entity entity)
{
    if (!registry.valid(entity)) return JPH::BodyID();
    if (!registry.all_of<TransformComponent, ColliderComponent, TriggerComponent>(entity)) return JPH::BodyID();
    if (!initialized) initialize();
    const auto& collider = registry.get<ColliderComponent>(entity);
    if (collider.shape_type == ColliderShapeType::Mesh || collider.shape_type == ColliderShapeType::ConvexHull)
    {
        LOG_ENGINE_WARN("createTriggerBody: Mesh/ConvexHull trigger shapes are not supported in v1 (primitive shapes only)");
        return JPH::BodyID();
    }
    const bool already = std::find(trigger_entities.begin(), trigger_entities.end(), entity) != trigger_entities.end();
    const uint32_t cap = std::min(settings.max_trigger_count, kMaxTriggerCount);
    if (!already && static_cast<uint32_t>(trigger_entities.size()) >= cap)
    {
        LOG_ENGINE_ERROR("createTriggerBody: trigger cap ({}) reached, failing closed", cap);
        return JPH::BodyID();
    }
    if (entity_to_body.find(entity) != entity_to_body.end())
        removeBody(entity);
    const auto& transform = registry.get<TransformComponent>(entity);
    JPH::ShapeRefC shape = createShapeFromCollider(collider, transform.scale);
    if (!shape) return JPH::BodyID();
    JPH::EMotionType motion = JPH::EMotionType::Static;
    if (registry.all_of<RigidBodyComponent>(entity) &&
        registry.get<RigidBodyComponent>(entity).motion_type == BodyMotionType::Kinematic)
        motion = JPH::EMotionType::Kinematic;
    JPH::BodyCreationSettings body_settings(shape, toJoltR(transform.position), toJoltQuat(transform.rotation),
        motion, settings.layers.trigger_body);
    body_settings.mIsSensor = true;
    body_settings.mFriction = collider.friction;
    body_settings.mRestitution = collider.restitution;
    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(body_settings);
    if (!body) return JPH::BodyID();
    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, motion == JPH::EMotionType::Kinematic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;
    trigger_entities.push_back(entity);
    registry.get_or_emplace<TriggerOverlapState>(entity);
    return body_id;
}

void PhysicsSystem::updateTriggers(entt::registry& registry)
{
    if (!initialized) return;
    // Snapshot trigger list (handlers are queued, not invoked here, so no
    // mutation happens during this loop; snapshot keeps it robust anyway).
    std::vector<entt::entity> triggers;
    auto view = registry.view<TriggerComponent, TransformComponent, ColliderComponent>();
    for (auto e : view) triggers.push_back(e);
    std::sort(triggers.begin(), triggers.end(),
        [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });

    if (triggers.empty()) return;

    // A TriggerComponent is authoring data, not proof that its sensor body was
    // created. Keep failed bodies (unsupported shapes, cap, allocation) fail-
    // closed, while retaining overlap state for successfully-created triggers
    // whose body was later removed so their occupants still receive exits.

    QueryFilter occupant_filter;
    occupant_filter.layer_mask = PhysicsObjectLayers::layerBit(settings.layers.dynamic_body) |
                                 PhysicsObjectLayers::layerBit(settings.layers.character_body);
    occupant_filter.hit_triggers = false;

    for (entt::entity trig : triggers)
    {
        if (!registry.valid(trig)) continue;
        const auto& trigger = registry.get<TriggerComponent>(trig);
        // Only create overlap state after createTriggerBody succeeded. This
        // prevents rejected/capped TriggerComponents from generating events.
        if (!registry.all_of<TriggerOverlapState>(trig)) continue;
        auto& state = registry.get<TriggerOverlapState>(trig);
        std::vector<entt::entity> now;
        const bool has_sensor_body =
            entity_to_body.find(trig) != entity_to_body.end() &&
            std::find(trigger_entities.begin(), trigger_entities.end(), trig) != trigger_entities.end();
        if (trigger.enabled && has_sensor_body)
        {
            // Allocate lazily on the first live sensor query, then reuse this
            // full-capacity scratch buffer across triggers and physics steps.
            if (trigger_overlap_buffer.size() != settings.max_bodies)
                trigger_overlap_buffer.resize(settings.max_bodies);
            const uint32_t overlap_capacity = static_cast<uint32_t>(trigger_overlap_buffer.size());

            const auto& t = registry.get<TransformComponent>(trig);
            const auto& col = registry.get<ColliderComponent>(trig);
            uint32_t n = 0;
            const glm::vec3 as = glm::abs(t.scale);
            switch (col.shape_type)
            {
            case ColliderShapeType::Box:
                n = overlapBox(t.position, col.box_half_extents * as, t.rotation, occupant_filter,
                    trigger_overlap_buffer.data(), overlap_capacity);
                break;
            case ColliderShapeType::Sphere:
                n = overlapSphere(t.position, col.sphere_radius * std::max({as.x, as.y, as.z}), occupant_filter,
                    trigger_overlap_buffer.data(), overlap_capacity);
                break;
            case ColliderShapeType::Capsule:
                n = overlapCapsule(t.position, col.capsule_half_height * as.y,
                    col.capsule_radius * std::max(as.x, as.z), occupant_filter,
                    trigger_overlap_buffer.data(), overlap_capacity);
                break;
            case ColliderShapeType::Cylinder:
                n = overlapCapsule(t.position, col.cylinder_half_height * as.y,
                    col.cylinder_radius * std::max(as.x, as.z), occupant_filter,
                    trigger_overlap_buffer.data(), overlap_capacity);
                break;
            default:
                break;
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                entt::entity other = trigger_overlap_buffer[i].entity;
                if (other == trig || !registry.valid(other)) continue;
                if (std::find(now.begin(), now.end(), other) == now.end()) now.push_back(other);
            }
            std::sort(now.begin(), now.end(),
                [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
        }
        for (entt::entity other : now)
            if (std::find(state.occupants.begin(), state.occupants.end(), other) == state.occupants.end())
                EventBus::get().queue(TriggerEnteredEvent{trig, other, trigger.trigger_id});
        for (entt::entity other : state.occupants)
            if (std::find(now.begin(), now.end(), other) == now.end())
                EventBus::get().queue(TriggerExitedEvent{trig, other, trigger.trigger_id});
        state.occupants = std::move(now);
    }
}

bool PhysicsSystem::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal)
{
    (void)registry;
    RaycastResult result = raycastClosest(origin, direction, maxDistance);
    if (!result.hit)
        return false;

    hitPoint = result.hit_point;
    hitNormal = result.hit_normal;
    return true;
}

PhysicsSystem::RaycastResult PhysicsSystem::raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
    float maxDistance, entt::entity ignoredEntity)
{
    QueryFilter filter;
    filter.ignore = ignoredEntity;
    return raycastClosest(origin, direction, maxDistance, filter);
}
