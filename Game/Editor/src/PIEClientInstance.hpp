#pragma once

#include "world.hpp"
#include "LevelManager.hpp"
#include "Plugin/GameModuleLoader.hpp"
#include "InputManager.hpp"
#include "Threading/JobSystem.hpp"
#include "Events/EventBus.hpp"
#include "InputActionSystem.hpp"
#include "Graphics/SceneViewport.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

// Settle only an operation accepted for this owner. A blocked_by token belongs
// to another caller and must be left for that caller to acknowledge.
inline bool finishOwnedRetirement(world& owner, LifecycleResult& result)
{
    if (!result.operation.valid()) return true;
    if (!result.accepted) return false;
    const auto operation = result.operation;
    if (!result.cleanupComplete()) {
        const auto resumed = owner.resumeRetirement(operation);
        if (!resumed.accepted || resumed.operation != operation) return false;
        result = resumed;
    }
    if (!result.cleanupComplete()) return false;
    const auto ack = owner.acknowledgeRetirement(operation);
    if (!ack.accepted || ack.operation != operation || !ack.cleanupComplete()) return false;
    result = {};
    return true;
}

// Retain construction evidence until its world-owned rollback is acknowledged.
inline bool finishLevelRollback(world& owner, LevelInstantiationResult& construction)
{
    auto& rollback = construction.rollback;
    if (!rollback.operation.valid()) return rollback.cleanupComplete();
    return finishOwnedRetirement(owner, rollback);
}

// Represents one additional PIE client instance (Player 2, 3, 4).
// Player 1 always uses the editor's main world and DLL load.
// Each additional client gets a separate DLL copy (isolated statics),
// its own world, input manager, and render target.
struct PIEClientInstance
{
    ~PIEClientInstance()
    {
        if (!release())
        {
            std::fprintf(stderr, "PIE: Player %d module retirement refused during destruction\n", player_index);
            std::_Exit(EXIT_FAILURE);
        }
    }

    int player_index = 0;                             // 2, 3, 4...
    world client_world;                               // isolated ECS + physics
    LevelInstantiationResult level_construction{};             // retained rollback for this world
    LifecycleResult reset_result{};                   // retained reset/teardown progress
    GameModuleLoader game_module;                     // separate DLL copy = isolated statics
    std::shared_ptr<InputManager> input_manager;      // per-client input
    InputActionSystem input_actions;                  // host-owned action state for this client
    EngineServices services{};                        // passed to game DLL init

    // Caller-owned scene viewport. Created via render_api->createSceneViewport.
    // Destruction routes resources through the API's deferred-release ring.
    std::unique_ptr<SceneViewport> viewport;
    int viewport_width = 640;
    int viewport_height = 480;
    bool initialized = false;
    bool released = false;

    // Destroy all client-owned state while its module image is still loaded.
    // The shared reflection and GameMode registrations belong to the provider.
    [[nodiscard]] bool release()
    {
        if (released) return true;
        if (!finishLevelRollback(client_world, level_construction)) return false;
        if (!finishOwnedRetirement(client_world, reset_result)) return false;
        if (initialized)
        {
            game_module.shutdown();
            initialized = false;
        }
        Threading::JobSystem::get().barrier();
        viewport.reset();
        input_manager.reset();
        const auto shutdown = client_world.shutdown();
        if (!shutdown.cleanupComplete()) {
            reset_result = shutdown;
            return false;
        }
        if (shutdown.operation.valid()) {
            reset_result = shutdown;
            if (!finishOwnedRetirement(client_world, reset_result)) return false;
        }
        reset_result = {};
        const auto* owner = game_module.getComponentOwnerId();
        if (Threading::JobSystem::get().getPendingJobCount() != 0 ||
            (owner && !EventBus::moduleSourceCanRetire(*owner)))
            return false;
        if (!game_module.unload()) return false;
        released = true;
        return true;
    }

    std::string window_title;                         // "Player 2", etc.
};
