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
    class GameModeBase;
    class GameStateBase;
}

class ENGINE_API world
{
private:
    std::unique_ptr<PhysicsSystem> physics_system;
    std::unique_ptr<GameFramework::GameModeBase> authority_game_mode;
    std::unique_ptr<GameFramework::GameStateBase> game_state;
    Tick::FixedTickAccumulator simulation_ticks;
    uint32_t simulation_tick = 0;
    bool gameplay_framework_initialized = false;
    bool gameplay_framework_started = false;

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

    void setAuthorityGameMode(std::unique_ptr<GameFramework::GameModeBase> game_mode);
    GameFramework::GameModeBase* getAuthorityGameMode() { return authority_game_mode.get(); }
    const GameFramework::GameModeBase* getAuthorityGameMode() const { return authority_game_mode.get(); }

    template<typename T>
    T* getAuthorityGameModeAs()
    {
        return dynamic_cast<T*>(authority_game_mode.get());
    }

    template<typename T>
    const T* getAuthorityGameModeAs() const
    {
        return dynamic_cast<const T*>(authority_game_mode.get());
    }

    void setGameState(std::unique_ptr<GameFramework::GameStateBase> state);
    GameFramework::GameStateBase* getGameState() { return game_state.get(); }
    const GameFramework::GameStateBase* getGameState() const { return game_state.get(); }

    template<typename T>
    T* getGameStateAs()
    {
        return dynamic_cast<T*>(game_state.get());
    }

    template<typename T>
    const T* getGameStateAs() const
    {
        return dynamic_cast<const T*>(game_state.get());
    }

    void clearGameplayFramework();
    bool initializeGameplayFramework(const std::string& map_name = "", const std::string& options = "");
    void startGameplayFramework();
    void tickGameplayFramework(float delta_time);
    void shutdownGameplayFramework();
    bool isGameplayFrameworkInitialized() const { return gameplay_framework_initialized; }
    bool hasGameplayFrameworkStarted() const { return gameplay_framework_started; }

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
        }

        return steps;
    }

    uint32_t getSimulationTick() const { return simulation_tick; }

    float getPhysicsInterpolationAlpha() const { return simulation_ticks.getAlpha(); }

    void player_collisions(entt::entity playerEntity)
    {
        physics_system->handlePlayerCollisions(registry, playerEntity);
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

    JPH::BodyID create_character_controller(entt::entity entity)
    {
        return physics_system->createCharacterController(registry, entity);
    }

    CharacterControllerState simulate_character_controller(entt::entity entity,
        const CharacterMoveInput& input, float delta_time)
    {
        return physics_system->simulateCharacterController(registry, entity, input, delta_time);
    }

    CharacterControllerState get_character_controller_state(entt::entity entity)
    {
        return physics_system->getCharacterControllerState(registry, entity);
    }

    bool set_character_controller_state(entt::entity entity, const CharacterControllerState& state)
    {
        return physics_system->setCharacterControllerState(registry, entity, state);
    }

    bool teleport_character_controller(entt::entity entity, const glm::vec3& position)
    {
        return physics_system->teleportCharacterController(registry, entity, position);
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
        return true;
    }

    void shutdown();

    // Full reset: tear down physics, clear ECS, reinitialize.
    // Used by the editor to restore pre-play state.
    void resetWorld();
};
