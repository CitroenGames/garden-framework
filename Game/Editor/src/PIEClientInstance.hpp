#pragma once

#include "world.hpp"
#include "Plugin/GameModuleLoader.hpp"
#include "InputManager.hpp"
#include "Graphics/SceneViewport.hpp"
#include <memory>
#include <string>

// Represents one additional PIE client instance (Player 2, 3, 4).
// Player 1 always uses the editor's main world and DLL load.
// Each additional client gets a separate DLL copy (isolated statics),
// its own world, input manager, and render target.
struct PIEClientInstance
{
    int player_index = 0;                             // 2, 3, 4...
    world client_world;                               // isolated ECS + physics
    GameModuleLoader game_module;                     // separate DLL copy = isolated statics
    std::shared_ptr<InputManager> input_manager;      // per-client input
    EngineServices services{};                        // passed to game DLL init

    // Caller-owned scene viewport. Created via render_api->createSceneViewport.
    // Destruction routes resources through the API's deferred-release ring.
    std::unique_ptr<SceneViewport> viewport;
    int viewport_width = 640;
    int viewport_height = 480;
    bool initialized = false;

    std::string window_title;                         // "Player 2", etc.
};
