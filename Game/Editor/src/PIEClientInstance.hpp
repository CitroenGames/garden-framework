#pragma once

#include "world.hpp"
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

    // Destroy all client-owned state while its module image is still loaded.
    // The shared reflection and GameMode registrations belong to the provider.
    [[nodiscard]] bool release()
    {
        if (initialized)
        {
            game_module.shutdown();
            initialized = false;
        }
        Threading::JobSystem::get().barrier();
        viewport.reset();
        input_manager.reset();
        client_world.shutdown();
        const auto* owner = game_module.getComponentOwnerId();
        if (Threading::JobSystem::get().getPendingJobCount() != 0 ||
            (owner && !EventBus::moduleSourceCanRetire(*owner)))
            return false;
        return game_module.unload();
    }

    std::string window_title;                         // "Player 2", etc.
};
