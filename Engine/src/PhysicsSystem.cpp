#include "PhysicsSystem.hpp"
#include "Utils/Log.hpp"
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>

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

PhysicsSystem::PhysicsSystem(const glm::vec3& gravityVector, float deltaTime)
    : gravity(gravityVector), fixed_delta(deltaTime)
{
}

PhysicsSystem::~PhysicsSystem()
{
    shutdown();
}

void PhysicsSystem::initialize()
{
    if (initialized) return;

    // Register Jolt allocators and types (once globally)
    if (!s_jolt_registered)
    {
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

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        s_jolt_registered = true;
    }

    // Create allocator and job system
    temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024); // 10 MB
    job_system = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

    // Create physics system
    const unsigned int cMaxBodies = 1024;
    const unsigned int cNumBodyMutexes = 0; // default
    const unsigned int cMaxBodyPairs = 1024;
    const unsigned int cMaxContactConstraints = 1024;

    jolt_system = std::make_unique<JPH::PhysicsSystem>();
    jolt_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
        broad_phase_layer_interface, object_vs_broadphase_filter, object_layer_pair_filter);

    // Set gravity (Jolt uses a much larger scale, our gravity is a unit direction * magnitude)
    // The old system used gravity as acceleration added per frame: velocity += gravity * delta
    // Jolt expects gravity in m/s^2. Our old gravity (0,-1,0) with delta 0.16 gave ~6.25 m/s^2 effective.
    // Let's scale to realistic: -9.81 m/s^2 on Y
    jolt_system->SetGravity(JPH::Vec3(gravity.x * 9.81f, gravity.y * 9.81f, gravity.z * 9.81f));

    // Set up contact listener for collision events
    contact_listener = std::make_unique<EngineContactListener>();
    contact_listener->setBodyToEntityMap(&body_to_entity);
    jolt_system->SetContactListener(contact_listener.get());

    initialized = true;
    LOG_ENGINE_INFO("Jolt Physics initialized ({} threads)", std::thread::hardware_concurrency() - 1);
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
    if (jolt_system)
    {
        jolt_system->SetGravity(JPH::Vec3(gravity.x * 9.81f, gravity.y * 9.81f, gravity.z * 9.81f));
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

JPH::BodyID PhysicsSystem::createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    if (!initialized) initialize();

    JPH::BodyCreationSettings settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Static, Layers::NON_MOVING);

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::DontActivate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    return body_id;
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

JPH::BodyID PhysicsSystem::createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity,
    float friction, float restitution, bool lock_rotation)
{
    if (!initialized) initialize();

    JPH::BodyCreationSettings settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Dynamic, Layers::MOVING);
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
    if (lock_rotation)
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast; // CCD to prevent tunneling
    settings.mFriction = friction;
    settings.mRestitution = restitution;
    settings.mLinearDamping = 0.0f; // Disable Jolt damping — game code handles velocity damping

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::Activate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    LOG_ENGINE_INFO("Created Jolt dynamic body at ({}, {}, {}), mass={}", position.x, position.y, position.z, mass);
    return body_id;
}

JPH::BodyID PhysicsSystem::createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    if (!initialized) initialize();

    JPH::BodyCreationSettings settings(shape, toJoltR(position), toJoltQuat(rotation), JPH::EMotionType::Kinematic, Layers::MOVING);
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::Activate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    LOG_ENGINE_INFO("Created Jolt kinematic body at ({}, {}, {})", position.x, position.y, position.z);
    return body_id;
}

JPH::BodyID PhysicsSystem::createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity)
{
    if (!initialized) initialize();
    if (!colliderMesh.vertices || colliderMesh.vertices_len < 3) return JPH::BodyID();

    // Build Jolt triangle list from mesh vertices (in local space)
    JPH::TriangleList triangles;
    triangles.reserve(colliderMesh.vertices_len / 3);

    for (size_t i = 0; i + 2 < colliderMesh.vertices_len; i += 3)
    {
        const vertex& v0 = colliderMesh.vertices[i];
        const vertex& v1 = colliderMesh.vertices[i + 1];
        const vertex& v2 = colliderMesh.vertices[i + 2];

        triangles.push_back(JPH::Triangle(
            JPH::Float3(v0.vx, v0.vy, v0.vz),
            JPH::Float3(v1.vx, v1.vy, v1.vz),
            JPH::Float3(v2.vx, v2.vy, v2.vz)
        ));
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

    // Apply scale via ScaledShape if not identity
    JPH::ShapeRefC final_shape = result.Get();
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

    return createStaticBody(position, rotation, final_shape, entity);
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
    JPH::SphereShapeSettings sphere(radius);
    auto shape_result = sphere.Create();
    if (!shape_result.IsValid()) return {};

    glm::vec3 dir = glm::normalize(direction) * maxDistance;
    return shapeCast(shape_result.Get(), origin, glm::vec3(0), dir);
}

PhysicsSystem::ShapeCastResult PhysicsSystem::boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
    const glm::vec3& rotation, const glm::vec3& direction, float maxDistance)
{
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

    // Sync ECS -> Jolt for dynamic bodies (in case game code moved them)
    syncTransformsToJolt(registry);

    // Step Jolt physics
    const int cCollisionSteps = 1;
    jolt_system->Update(fixed_delta, cCollisionSteps, temp_allocator.get(), job_system.get());

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

        auto& rb = view.get<RigidBodyComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);

        if (rb.apply_gravity)
            rb.velocity += gravity * 9.81f * fixed_delta;

        rb.velocity += rb.force * fixed_delta;
        transform.position += rb.velocity * fixed_delta;
        rb.force = glm::vec3(0);
    }
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

        LOG_ENGINE_TRACE("[FromJolt] pos=({},{},{}) vel=({},{},{})",
            transform.position.x, transform.position.y, transform.position.z,
            rb.velocity.x, rb.velocity.y, rb.velocity.z);
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

        LOG_ENGINE_TRACE("[ToJolt] pushing vel=({},{},{})",
            rb.velocity.x, rb.velocity.y, rb.velocity.z);

        // Validate and clamp velocity before pushing to Jolt
        if (!isValidVec3(rb.velocity))
        {
            LOG_ENGINE_WARN("NaN velocity detected, resetting to zero");
            rb.velocity = glm::vec3(0);
        }
        rb.velocity = clampVelocity(rb.velocity);
        body_interface.SetLinearVelocity(body_id, toJolt(rb.velocity));

        // If game code applied forces, send them to Jolt (force = acceleration * mass)
        if (glm::length(rb.force) > 0.001f)
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
    if (!registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(playerEntity)) return;

    auto& player = registry.get<PlayerComponent>(playerEntity);

    // Check if player has a Jolt body
    auto it = entity_to_body.find(playerEntity);

    if (it != entity_to_body.end())
    {
        // Ground detection: cast a ray downward, excluding the player's own body
        auto& transform = registry.get<TransformComponent>(playerEntity);
        // Ray length derived from capsule dimensions + tolerance
        float ground_ray_length = player.capsule_half_height + player.capsule_radius + 0.3f;
        JPH::RRayCast ray(toJoltR(transform.position), JPH::Vec3(0, -1, 0) * ground_ray_length);
        JPH::RayCastResult hit;
        JPH::IgnoreSingleBodyFilter body_filter(it->second);

        player.grounded = jolt_system->GetNarrowPhaseQuery().CastRay(
            ray, hit,
            JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::NON_MOVING),
            JPH::SpecifiedObjectLayerFilter(Layers::NON_MOVING),
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
            if (player.ground_normal.y < 0.7f)
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

bool PhysicsSystem::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal)
{
    if (!initialized) return false;

    JPH::Vec3 dir = toJolt(glm::normalize(direction)) * maxDistance;
    JPH::RRayCast ray(toJoltR(origin), dir);
    JPH::RayCastResult hit;

    if (jolt_system->GetNarrowPhaseQuery().CastRay(ray, hit))
    {
        JPH::RVec3 hitPos = ray.GetPointOnRay(hit.mFraction);
        hitPoint = glm::vec3(float(hitPos.GetX()), float(hitPos.GetY()), float(hitPos.GetZ()));

        // Get the hit normal from the body
        JPH::BodyLockRead lock(jolt_system->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
            hitNormal = toGlm(normal);
        }
        else
        {
            hitNormal = glm::vec3(0, 1, 0);
        }
        return true;
    }

    return false;
}
