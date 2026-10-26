#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <cstdint>
#include <glm/glm.hpp>

namespace PhysicsObjectLayers
{
    // D6: layers 2 -> 5. Values 0/1 unchanged (StaticWorld/DynamicProp),
    // so any persisted 0/1 maps 0->0, 1->1 (D6-U1 found none persisted).
    static constexpr JPH::ObjectLayer StaticWorld = 0;         // was Static
    static constexpr JPH::ObjectLayer DynamicProp = 1;         // was Dynamic
    static constexpr JPH::ObjectLayer Character = 2;           // NEW
    static constexpr JPH::ObjectLayer Trigger = 3;             // NEW (sensor)
    static constexpr JPH::ObjectLayer ProjectileQueryOnly = 4; // NEW (query-mask only)
    static constexpr JPH::ObjectLayer Count = 5;
    // Back-compatible aliases.
    static constexpr JPH::ObjectLayer Static = StaticWorld;
    static constexpr JPH::ObjectLayer Dynamic = DynamicProp;
    inline constexpr uint32_t layerBit(JPH::ObjectLayer layer)
    {
        return layer < 32 ? (uint32_t(1) << uint32_t(layer)) : 0u;
    }
}

namespace PhysicsBroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer Static(0);
    static constexpr JPH::BroadPhaseLayer Dynamic(1);
    static constexpr uint32_t Count = 2;
}

struct PhysicsLayerSettings
{
    JPH::ObjectLayer static_body = PhysicsObjectLayers::StaticWorld;
    JPH::ObjectLayer dynamic_body = PhysicsObjectLayers::DynamicProp;
    JPH::ObjectLayer character_body = PhysicsObjectLayers::Character;
    JPH::ObjectLayer trigger_body = PhysicsObjectLayers::Trigger;
    JPH::ObjectLayer projectile_query_body = PhysicsObjectLayers::ProjectileQueryOnly;
    JPH::ObjectLayer object_layer_count = PhysicsObjectLayers::Count;

    JPH::BroadPhaseLayer static_broad_phase = PhysicsBroadPhaseLayers::Static;
    JPH::BroadPhaseLayer dynamic_broad_phase = PhysicsBroadPhaseLayers::Dynamic;
    uint32_t broad_phase_layer_count = PhysicsBroadPhaseLayers::Count;

    bool shouldObjectCollideWithBroadPhase(JPH::ObjectLayer object_layer, JPH::BroadPhaseLayer broad_phase_layer) const
    {
        if (object_layer == trigger_body || object_layer == projectile_query_body)
            return false;
        if (object_layer == static_body)
            return broad_phase_layer == dynamic_broad_phase;
        if (object_layer == dynamic_body || object_layer == character_body)
            return true;
        return false;
    }

    bool shouldObjectsCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const
    {
        // Trigger and ProjectileQueryOnly collide with nothing (sensor / query-only).
        if (a == trigger_body || b == trigger_body ||
            a == projectile_query_body || b == projectile_query_body)
            return false;
        if (a == static_body)
            return b == dynamic_body || b == character_body;
        if (a == dynamic_body)
            return b == static_body || b == dynamic_body || b == character_body;
        if (a == character_body)
            return b == static_body || b == dynamic_body || b == character_body;
        return false;
    }

    JPH::BroadPhaseLayer broadPhaseForObjectLayer(JPH::ObjectLayer object_layer) const
    {
        if (object_layer == static_body)
            return static_broad_phase;
        return dynamic_broad_phase;
    }
};

struct PhysicsSystemSettings
{
    PhysicsLayerSettings layers;

    glm::vec3 gravity_direction = glm::vec3(0.0f, -1.0f, 0.0f);
    float gravity_acceleration = 9.81f;
    float fixed_delta = 1.0f / 60.0f;

    uint32_t max_bodies = 1024;
    uint32_t body_mutex_count = 0;
    uint32_t max_body_pairs = 1024;
    uint32_t max_contact_constraints = 1024;
    uint32_t temp_allocator_size_bytes = 10u * 1024u * 1024u;
    uint32_t worker_thread_count = 0; // 0 means auto.
    uint32_t collision_steps = 1;

    float max_body_velocity = 100.0f;
    float max_character_velocity = 100.0f;
    float min_body_mass = 0.001f;
    float dynamic_linear_damping = 0.0f;
    float kinematic_gravity_factor = 0.0f;
    float force_epsilon = 0.001f;

    float mesh_degenerate_triangle_epsilon = 1.0e-12f;
    float player_ground_probe_padding = 0.3f;
    float player_min_ground_normal_y = 0.7f;
    float raycast_direction_epsilon = 0.0001f;
    uint32_t max_trigger_count = 256;
};
