#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include "Components/Components.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <unordered_map>
#include <memory>

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
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterMask.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

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

class PhysicsSystem
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

    bool initialized = false;

    // Helper: convert glm <-> Jolt types
    static JPH::Vec3 toJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    static JPH::RVec3 toJoltR(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
    static glm::vec3 toGlm(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

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

    // Body management
    JPH::BodyID createStaticBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, entt::entity entity);
    JPH::BodyID createDynamicBody(const glm::vec3& position, const glm::vec3& rotation, const JPH::ShapeRefC& shape, float mass, entt::entity entity);
    JPH::BodyID createStaticMeshBody(const glm::vec3& position, const glm::vec3& rotation, const mesh& colliderMesh, entt::entity entity);
    void removeBody(entt::entity entity);

    // Main physics update
    void stepPhysics(entt::registry& registry);

    // Collision detection and response
    void handlePlayerCollisions(entt::registry& registry, entt::entity playerEntity, float sphereRadius);

    // General collision queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        entt::registry& registry, glm::vec3& hitPoint, glm::vec3& hitNormal);

    // Access Jolt system
    JPH::PhysicsSystem* getJoltSystem() { return jolt_system.get(); }
    JPH::BodyInterface& getBodyInterface();

    // Sync ECS transforms from Jolt
    void syncTransformsFromJolt(entt::registry& registry);
    void syncTransformsToJolt(entt::registry& registry);
};
