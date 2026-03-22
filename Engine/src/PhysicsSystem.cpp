#include "PhysicsSystem.hpp"
#include "Utils/Log.hpp"
#include <cmath>
#include <cstdarg>
#include <cstdlib>

// Jolt memory allocation hooks
static void* JoltAllocate(size_t inSize) { return malloc(inSize); }
static void* JoltReallocate(void* inBlock, size_t /*inOldSize*/, size_t inNewSize) { return realloc(inBlock, inNewSize); }
static void* JoltAllocateAligned(size_t inSize, size_t inAlignment)
{
    void* ptr = nullptr;
    posix_memalign(&ptr, inAlignment, inSize);
    return ptr;
}
static void JoltFree(void* inBlock) { free(inBlock); }
static void JoltFreeAligned(void* inBlock) { free(inBlock); }

static void JoltTrace(const char* inFMT, ...)
{
    va_list args;
    va_start(args, inFMT);
    vprintf(inFMT, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static bool s_jolt_registered = false;

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

    initialized = true;
    LOG_ENGINE_INFO("Jolt Physics initialized ({} threads)", std::thread::hardware_concurrency() - 1);
}

void PhysicsSystem::shutdown()
{
    if (!initialized) return;

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

    jolt_system.reset();
    job_system.reset();
    temp_allocator.reset();

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

JPH::BodyID PhysicsSystem::createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity)
{
    if (!initialized) initialize();

    JPH::BodyCreationSettings settings(shape, toJoltR(position), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);

    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
    JPH::Body* body = body_interface.CreateBody(settings);
    if (!body) return JPH::BodyID();

    JPH::BodyID body_id = body->GetID();
    body_interface.AddBody(body_id, JPH::EActivation::DontActivate);

    entity_to_body[entity] = body_id;
    body_to_entity[body_id] = entity;

    return body_id;
}

JPH::BodyID PhysicsSystem::createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity)
{
    if (!initialized) initialize();

    JPH::BodyCreationSettings settings(shape, toJoltR(position), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::MOVING);
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
    settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ; // Lock rotation

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

JPH::BodyID PhysicsSystem::createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const mesh& colliderMesh, entt::entity entity)
{
    if (!initialized) initialize();
    if (!colliderMesh.vertices || colliderMesh.vertices_len < 3) return JPH::BodyID();

    // Build Jolt triangle list from mesh vertices
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

    LOG_ENGINE_INFO("Creating Jolt mesh body: {} triangles at ({}, {}, {})", triangles.size(), position.x, position.y, position.z);

    JPH::MeshShapeSettings mesh_settings(triangles);
    JPH::ShapeSettings::ShapeResult result = mesh_settings.Create();
    if (!result.IsValid())
    {
        LOG_ENGINE_ERROR("Failed to create Jolt mesh shape: {}", result.GetError().c_str());
        return JPH::BodyID();
    }

    return createStaticBody(position, rotation, result.Get(), entity);
}

void PhysicsSystem::removeBody(entt::entity entity)
{
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
            rb.velocity += gravity * fixed_delta;

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

        transform.position = glm::vec3(float(pos.GetX()), float(pos.GetY()), float(pos.GetZ()));
        rb.velocity = toGlm(vel);
    }
}

void PhysicsSystem::syncTransformsToJolt(entt::registry& registry)
{
    JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();

    for (auto& [entity, body_id] : entity_to_body)
    {
        if (!registry.valid(entity)) continue;
        if (!registry.all_of<TransformComponent, RigidBodyComponent>(entity)) continue;

        if (body_interface.GetMotionType(body_id) != JPH::EMotionType::Dynamic) continue;

        auto& rb = registry.get<RigidBodyComponent>(entity);

        // Push velocity from game code (PlayerController) to Jolt
        body_interface.SetLinearVelocity(body_id, toJolt(rb.velocity));

        // If game code applied forces, send them to Jolt
        if (glm::length(rb.force) > 0.001f)
        {
            body_interface.AddForce(body_id, toJolt(rb.force));
            rb.force = glm::vec3(0);
        }
    }
}

void PhysicsSystem::handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity, float sphereRadius)
{
    if (!initialized || !registry.valid(playerEntity)) return;
    if (!registry.all_of<TransformComponent, RigidBodyComponent, PlayerComponent>(playerEntity)) return;

    auto& player = registry.get<PlayerComponent>(playerEntity);

    // Check if player has a Jolt body
    auto it = entity_to_body.find(playerEntity);
    if (it != entity_to_body.end())
    {
        JPH::BodyInterface& body_interface = jolt_system->GetBodyInterface();
        JPH::Vec3 vel = body_interface.GetLinearVelocity(it->second);

        // Simple ground detection: cast a short ray downward
        auto& transform = registry.get<TransformComponent>(playerEntity);
        JPH::RRayCast ray(toJoltR(transform.position), JPH::Vec3(0, -1, 0) * (sphereRadius + 0.1f));
        JPH::RayCastResult hit;

        player.grounded = jolt_system->GetNarrowPhaseQuery().CastRay(ray, hit);
        if (player.grounded)
        {
            player.ground_normal = glm::vec3(0, 1, 0); // Simplified
        }
    }
    else
    {
        // Fallback: no Jolt body, just reset ground state
        player.grounded = false;
        player.ground_normal = glm::vec3(0, 1, 0);
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
