#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Character/CharacterControllerSystem.hpp"
#include "Components/Components.hpp"
#include "Physics/PhysicsSettings.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cmath>

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterMask.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include <mutex>

// BroadPhaseLayerInterface implementation
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    explicit BPLayerInterfaceImpl(const PhysicsLayerSettings& layer_settings = {})
        : layers(layer_settings) {}

    void setLayerSettings(const PhysicsLayerSettings& layer_settings) { layers = layer_settings; }

    virtual unsigned int GetNumBroadPhaseLayers() const override { return layers.broad_phase_layer_count; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        return layers.broadPhaseForObjectLayer(inLayer);
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        if (inLayer == layers.static_broad_phase) return "Static";
        if (inLayer == layers.dynamic_broad_phase) return "Dynamic";
        return "Invalid";
    }
#endif

private:
    PhysicsLayerSettings layers;
};

// ObjectVsBroadPhaseLayerFilter implementation
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    explicit ObjectVsBroadPhaseLayerFilterImpl(const PhysicsLayerSettings& layer_settings = {})
        : layers(layer_settings) {}

    void setLayerSettings(const PhysicsLayerSettings& layer_settings) { layers = layer_settings; }

    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        return layers.shouldObjectCollideWithBroadPhase(inLayer1, inLayer2);
    }

private:
    PhysicsLayerSettings layers;
};

// ObjectLayerPairFilter implementation
class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
{
public:
    explicit ObjectLayerPairFilterImpl(const PhysicsLayerSettings& layer_settings = {})
        : layers(layer_settings) {}

    void setLayerSettings(const PhysicsLayerSettings& layer_settings) { layers = layer_settings; }

    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
    {
        return layers.shouldObjectsCollide(inObject1, inObject2);
    }

private:
    PhysicsLayerSettings layers;
};

// Contact listener that queues CollisionEvents for main-thread dispatch
class EngineContactListener : public JPH::ContactListener
{
public:
    void setBodyToEntityMap(const std::unordered_map<JPH::BodyID, entt::entity>* map) { m_map = map; }

    virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
        const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override
    {
        if (!m_map) return;

        CollisionEvent evt;
        auto it1 = m_map->find(inBody1.GetID());
        auto it2 = m_map->find(inBody2.GetID());
        evt.entity_a = (it1 != m_map->end()) ? it1->second : entt::null;
        evt.entity_b = (it2 != m_map->end()) ? it2->second : entt::null;

        // Contact point (use base offset for world-space approximation)
        evt.contact_normal = glm::vec3(
            inManifold.mWorldSpaceNormal.GetX(),
            inManifold.mWorldSpaceNormal.GetY(),
            inManifold.mWorldSpaceNormal.GetZ());

        if (inManifold.mRelativeContactPointsOn1.size() > 0)
        {
            JPH::Vec3 world_pt = inManifold.mBaseOffset + inManifold.mRelativeContactPointsOn1[0];
            evt.contact_point = glm::vec3(world_pt.GetX(), world_pt.GetY(), world_pt.GetZ());
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(evt);
    }

    virtual void OnContactPersisted(const JPH::Body& /*inBody1*/, const JPH::Body& /*inBody2*/,
        const JPH::ContactManifold& /*inManifold*/, JPH::ContactSettings& /*ioSettings*/) override
    {
        // Only fire on new contacts, not persisted ones
    }

    // Drain queued events to EventBus (call from main thread after physics step)
    void drainEvents()
    {
        std::vector<CollisionEvent> events;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            events.swap(m_pending);
        }
        for (auto& e : events)
            EventBus::get().queue(std::move(e));
    }

private:
    const std::unordered_map<JPH::BodyID, entt::entity>* m_map = nullptr;
    std::mutex m_mutex;
    std::vector<CollisionEvent> m_pending;
};

class ENGINE_API PhysicsSystem
{
private:
    PhysicsSystemSettings settings;
    glm::vec3 gravity;
    float fixed_delta;

    // Jolt systems
    std::unique_ptr<JPH::PhysicsSystem> jolt_system;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;

    // Layer interfaces
    BPLayerInterfaceImpl broad_phase_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_filter;
    ObjectLayerPairFilterImpl object_layer_pair_filter;

    // Entity <-> Jolt body mapping
    std::unordered_map<entt::entity, JPH::BodyID> entity_to_body;
    std::unordered_map<JPH::BodyID, entt::entity> body_to_entity;

    // Contact listener
    std::unique_ptr<EngineContactListener> contact_listener;

    // Constraint management
    std::unordered_map<entt::entity, JPH::Ref<JPH::Constraint>> entity_to_constraint;

    CharacterControllerSystem character_controllers;

    bool initialized = false;

    // Helper: convert glm <-> Jolt types
    static JPH::Vec3 toJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    static JPH::RVec3 toJoltR(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
    static glm::vec3 toGlm(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

    // Convert engine Euler angles (degrees, YXZ order) to Jolt quaternion
    static JPH::Quat toJoltQuat(const glm::vec3& euler_degrees)
    {
        glm::mat4 rot = glm::eulerAngleYXZ(
            glm::radians(euler_degrees.y),
            glm::radians(euler_degrees.x),
            glm::radians(euler_degrees.z));
        glm::quat q = glm::quat_cast(rot);
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    // Safety helpers
    static bool isValidVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    glm::vec3 clampVelocity(const glm::vec3& v) const;
    glm::vec3 gravityAcceleration() const { return gravity * settings.gravity_acceleration; }
    void applySettings(const PhysicsSystemSettings& new_settings);

public:
    struct RaycastResult {
        bool hit = false;
        entt::entity entity = entt::null;
        glm::vec3 hit_point{0.0f};
        glm::vec3 hit_normal{0.0f, 1.0f, 0.0f};
        float fraction = 1.0f;
        float distance = 0.0f;
    };

    struct PhysicsBodyDesc
    {
        float mass = 1.0f;
        float friction = 0.2f;
        float restitution = 0.0f;
        bool apply_gravity = true;
        bool lock_rotation = true;
    };

    explicit PhysicsSystem(const PhysicsSystemSettings& settings);
    PhysicsSystem(const glm::vec3& gravityVector = glm::vec3(0, -1, 0), float deltaTime = 1.0f / 60.0f);
    ~PhysicsSystem();

    void initialize();
    void shutdown();

    // Getters and setters
    void setGravity(const glm::vec3& gravityVector);
    glm::vec3 getGravity() const { return gravity; }
    float getGravityAcceleration() const { return settings.gravity_acceleration; }
    const PhysicsSystemSettings& getSettings() const { return settings; }
    bool configure(const PhysicsSystemSettings& new_settings);

    void setFixedDelta(float deltaTime)
    {
        fixed_delta = deltaTime > 0.0f ? deltaTime : 0.0001f;
        settings.fixed_delta = fixed_delta;
    }
    float getFixedDelta() const { return fixed_delta; }

    // Shape creation
    static void ensureJoltRegistered();
    static JPH::ShapeRefC createShapeFromCollider(const ColliderComponent& collider, const glm::vec3& scale);

    // Body management
    JPH::BodyID createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
        const PhysicsBodyDesc& desc);
    JPH::BodyID createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
        const PhysicsBodyDesc& desc);
    JPH::BodyID createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity,
        float friction = 0.0f, float restitution = 0.0f, bool lock_rotation = true);
    JPH::BodyID createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity,
        const PhysicsBodyDesc& desc);
    JPH::BodyID createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity);
    JPH::BodyID createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity,
        const PhysicsBodyDesc& desc);
    JPH::BodyID createPlayerBody(entt::registry& registry, entt::entity entity);
    void removeBody(entt::entity entity);
    bool hasBody(entt::entity entity) const { return entity_to_body.find(entity) != entity_to_body.end(); }

    // Character controller management
    JPH::BodyID createCharacterController(entt::registry& registry, entt::entity entity);
    void removeCharacterController(entt::entity entity);
    bool hasCharacterController(entt::entity entity) const { return character_controllers.has(entity); }
    CharacterControllerState simulateCharacterController(entt::registry& registry, entt::entity entity,
        const CharacterMoveInput& input, float delta_time);
    CharacterControllerState getCharacterControllerState(entt::registry& registry, entt::entity entity) const;
    bool setCharacterControllerState(entt::registry& registry, entt::entity entity,
        const CharacterControllerState& state);
    bool teleportCharacterController(entt::registry& registry, entt::entity entity, const glm::vec3& position);

    // Main physics update
    void stepPhysics(entt::registry& registry);

    // Collision detection and response
    void handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity);

    // General collision queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal);
    RaycastResult raycastClosest(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        entt::entity ignoredEntity = entt::null);

    // Shape casting result
    struct ShapeCastResult {
        bool hit = false;
        entt::entity entity = entt::null;
        glm::vec3 contact_point{0.0f};
        glm::vec3 contact_normal{0.0f};
        float fraction = 1.0f;
    };

    // Shape casting queries
    ShapeCastResult shapeCast(const JPH::ShapeRefC& shape, const glm::vec3& position,
        const glm::vec3& rotation, const glm::vec3& direction);
    ShapeCastResult sphereCast(const glm::vec3& origin, float radius,
        const glm::vec3& direction, float maxDistance);
    ShapeCastResult boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
        const glm::vec3& rotation, const glm::vec3& direction, float maxDistance);

    // Constraint management
    JPH::Constraint* createConstraint(entt::entity entityA, entt::entity entityB, const ConstraintComponent& constraint);
    void removeConstraint(entt::entity entity);

    // Access Jolt system
    JPH::PhysicsSystem* getJoltSystem() { return jolt_system.get(); }
    JPH::BodyInterface& getBodyInterface();

    // Broad phase optimization (call after loading a level)
    void optimizeBroadPhase();

    // Sync ECS transforms from Jolt
    void syncTransformsFromJolt(entt::registry& registry);
    void syncTransformsToJolt(entt::registry& registry);
};
