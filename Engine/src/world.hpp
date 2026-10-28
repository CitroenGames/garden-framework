#pragma once

#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "EngineExport.h"
#include "PhysicsSystem.hpp"
#include "Tick/TickSystem.hpp"
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <entt/entt.hpp>

namespace GameFramework
{
    class GameFrameworkRuntime;
    class GameModeBase;
    class GameStateBase;
}

class ENGINE_API world
{
private:
    // CharacterPhysicsService implements its Garden handle API through this private seam.
    friend class CharacterPhysicsService;
    std::unique_ptr<PhysicsSystem> physics_system;
    std::unique_ptr<GameFramework::GameFrameworkRuntime> gameplay_framework;
    Tick::FixedTickAccumulator simulation_ticks;
    uint32_t simulation_tick = 0;
    double simulation_time = 0.0;

    // Private bridge from Garden identity to the ECS runtime.
    entt::entity resolveEntity(EntityRef entity) const;

    void clearRegistryStorage()
    {
        registry.clear();

        std::vector<entt::id_type> storage_ids;
        for (auto&& [id, storage] : registry.storage())
            storage_ids.push_back(id);

        for (entt::id_type id : storage_ids)
            registry.reset(id);
    }

public:
    entt::registry registry;
    camera world_camera;
    float fixed_delta;

    explicit world(const PhysicsSystemSettings& physics_settings = PhysicsSystemSettings());

    world(world&&) noexcept;
    world& operator=(world&&) noexcept;
    world(const world&) = delete;
    world& operator=(const world&) = delete;

    ~world();

    // Creates a Garden entity handle with a stable identity. Gameplay code uses
    // this handle instead of storage-library entity values.
    EntityRef createEntity();

    // Returned component pointers may be invalidated by structural changes to
    // that component type's storage, including adding/removing it on another
    // entity, or by destroying the owning entity. Invalid handles and absent
    // components return null. Callers must not retain pointers across such
    // changes.
    bool isEntityValid(EntityRef entity) const;

    template<typename T>
    bool hasComponent(EntityRef entity) const
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null && registry.all_of<T>(resolved);
    }

    template<typename T>
    T* tryGetComponent(EntityRef entity)
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null ? registry.try_get<T>(resolved) : nullptr;
    }

    template<typename T>
    const T* tryGetComponent(EntityRef entity) const
    {
        const auto resolved = resolveEntity(entity);
        return resolved != entt::null ? registry.try_get<T>(resolved) : nullptr;
    }

    // Returns the uniquely named entity's existing identity, or a null handle
    // if the name is empty, absent, ambiguous, or has no non-null identity.
    EntityRef findEntityByTag(const std::string& name) const;

    void setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> game_mode);
    GameFramework::GameModeBase* getAuthorityGameMode();
    const GameFramework::GameModeBase* getAuthorityGameMode() const;

    template<typename T>
    T* getAuthorityGameModeAs()
    {
        return dynamic_cast<T*>(getAuthorityGameMode());
    }

    template<typename T>
    const T* getAuthorityGameModeAs() const
    {
        return dynamic_cast<const T*>(getAuthorityGameMode());
    }

    void setGameState(std::unique_ptr<GameFramework::GameStateBase> state);
    GameFramework::GameStateBase* getGameState();
    const GameFramework::GameStateBase* getGameState() const;

    template<typename T>
    T* getGameStateAs()
    {
        return dynamic_cast<T*>(getGameState());
    }

    template<typename T>
    const T* getGameStateAs() const
    {
        return dynamic_cast<const T*>(getGameState());
    }

    void clearGameplayFramework();
    bool initializeGameplayFramework(const std::string& map_name = "", const std::string& options = "");
    void startGameplayFramework();
    void tickGameplayFramework(float delta_time);
    void shutdownGameplayFramework();
    bool isGameplayFrameworkInitialized() const;
    bool hasGameplayFrameworkStarted() const;

    // Initialize Jolt physics (call after world construction)
    void initializePhysics()
    {
        physics_system->initialize();
    }

    // Physics system access
    PhysicsSystem& getPhysicsSystem() { return *physics_system; }
    const PhysicsSystem& getPhysicsSystem() const { return *physics_system; }

    // Simplified interface that delegates to physics system
    uint32_t step_physics(float dt)
    {
        simulation_ticks.setFixedDelta(fixed_delta);
        const uint32_t steps = simulation_ticks.consume(dt);

        for (uint32_t i = 0; i < steps; ++i)
        {
            physics_system->stepPhysics(registry);
            ++simulation_tick;
            simulation_time += fixed_delta;
        }

        return steps;
    }

    // One fixed physics step without accumulator consumption.
    // Used by SystemScheduler's Physics phase, which owns accumulation.
    void stepPhysicsSingleStep()
    {
        physics_system->stepPhysics(registry);
        ++simulation_tick;
        simulation_time += fixed_delta;
    }

    uint32_t getSimulationTick() const { return simulation_tick; }
    double getWorldTime() const { return simulation_time; }
    float getFixedDelta() const { return fixed_delta; }
    Tick::SimulationTickContext getTickContext(uint32_t substep_index, uint32_t substep_count) const
    {
        Tick::SimulationTickContext context;
        context.tick_index = simulation_tick + substep_index;
        context.substep_index = substep_index;
        context.substep_count = substep_count;
        context.fixed_delta = fixed_delta;
        context.world_time = simulation_time + static_cast<double>(substep_index) * fixed_delta;
        return context;
    }

    float getPhysicsInterpolationAlpha() const { return simulation_ticks.getAlpha(); }

    // D6 trigger polling (also invoked automatically at end of step_physics;
    // calling again is a no-change diff, so scheduler systems stay exactly-once).
    void update_triggers()
    {
        physics_system->updateTriggers(registry);
    }

    PhysicsSystem::RaycastResult raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
        float max_distance, const PhysicsSystem::QueryFilter& filter)
    {
        return physics_system->raycastClosest(origin, direction, max_distance, filter);
    }

    uint32_t overlapSphere(const glm::vec3& center, float radius,
        const PhysicsSystem::QueryFilter& filter, PhysicsSystem::OverlapResult* out, uint32_t cap)
    {
        return physics_system->overlapSphere(center, radius, filter, out, cap);
    }

    // Utility methods for common physics queries
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float max_distance,
        glm::vec3& hit_point, glm::vec3& hit_normal)
    {
        return physics_system->raycast(origin, direction, max_distance, registry, hit_point, hit_normal);
    }

    PhysicsSystem::RaycastResult raycastClosest(const glm::vec3& origin, const glm::vec3& direction,
        float max_distance, entt::entity ignored_entity = entt::null)
    {
        return physics_system->raycastClosest(origin, direction, max_distance, ignored_entity);
    }

    // Shape casting convenience wrappers
    PhysicsSystem::ShapeCastResult sphereCast(const glm::vec3& origin, float radius,
        const glm::vec3& direction, float maxDistance)
    {
        return physics_system->sphereCast(origin, radius, direction, maxDistance);
    }

    PhysicsSystem::ShapeCastResult boxCast(const glm::vec3& origin, const glm::vec3& halfExtents,
        const glm::vec3& rotation, const glm::vec3& direction, float maxDistance)
    {
        return physics_system->boxCast(origin, halfExtents, rotation, direction, maxDistance);
    }

    bool teleport_body(entt::entity entity, const glm::vec3& position, const glm::vec3& euler_degrees)
    {
        return physics_system->teleportBody(registry, entity, position, euler_degrees);
    }

    // Configuration methods
    void setGravity(const glm::vec3& gravity)
    {
        physics_system->setGravity(gravity);
    }

    glm::vec3 getGravity() const
    {
        return physics_system->getGravity();
    }

    void setFixedDelta(float delta)
    {
        physics_system->setFixedDelta(delta);
        fixed_delta = physics_system->getFixedDelta();
        simulation_ticks.setFixedDelta(fixed_delta);
    }

    bool configurePhysics(const PhysicsSystemSettings& physics_settings)
    {
        if (!physics_system->configure(physics_settings))
            return false;
        fixed_delta = physics_system->getFixedDelta();
        simulation_ticks.setFixedDelta(fixed_delta);
        simulation_ticks.reset();
        simulation_tick = 0;
        simulation_time = 0.0;
        return true;
    }

    void shutdown();

    // Full reset: tear down physics, clear ECS, reinitialize.
    // Used by the editor to restore pre-play state.
    void resetWorld();
};
