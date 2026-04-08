#pragma once

#include "EngineExport.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Components/Components.hpp"
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
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include <mutex>

// Jolt object layers
namespace Layers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
};

// BroadPhase layers
namespace BroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr unsigned int NUM_LAYERS = 2;
};

// BroadPhaseLayerInterface implementation
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        m_object_to_broadphase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_object_to_broadphase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    virtual unsigned int GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        return m_object_to_broadphase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        switch ((JPH::BroadPhaseLayer::Type)inLayer)
        {
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING: return "MOVING";
        default: return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer m_object_to_broadphase[Layers::NUM_LAYERS];
};

// ObjectVsBroadPhaseLayerFilter implementation
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case Layers::NON_MOVING:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

// ObjectLayerPairFilter implementation
class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
{
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
    {
        switch (inObject1)
        {
        case Layers::NON_MOVING:
            return inObject2 == Layers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
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
    static constexpr float MAX_VELOCITY = 100.0f; // m/s

    static bool isValidVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    static glm::vec3 clampVelocity(const glm::vec3& v)
    {
        float len = glm::length(v);
        if (len > MAX_VELOCITY)
            return v * (MAX_VELOCITY / len);
        return v;
    }

public:
    PhysicsSystem(const glm::vec3& gravityVector = glm::vec3(0, -1, 0), float deltaTime = 1.0f / 60.0f);
    ~PhysicsSystem();

    void initialize();
    void shutdown();

    // Getters and setters
    void setGravity(const glm::vec3& gravityVector);
    glm::vec3 getGravity() const { return gravity; }

    void setFixedDelta(float deltaTime) { fixed_delta = deltaTime; }
    float getFixedDelta() const { return fixed_delta; }

    // Shape creation
    static JPH::ShapeRefC createShapeFromCollider(const ColliderComponent& collider, const glm::vec3& scale);

    // Body management
    JPH::BodyID createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity,
        float friction = 0.0f, float restitution = 0.0f, bool lock_rotation = true);
    JPH::BodyID createKinematicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale, const mesh& colliderMesh, entt::entity entity);
    void removeBody(entt::entity entity);

    // Main physics update
    void stepPhysics(entt::registry& registry);

    // Collision detection and response
    void handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity);

    // General collision queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal);

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
