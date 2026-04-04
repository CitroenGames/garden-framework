#pragma once

#include <cstdint>

#define GARDEN_MODULE_API_VERSION 2

// Forward declarations
class world;
class IRenderAPI;
class InputManager;
class ReflectionRegistry;
class Application;
class LevelManager;

// Bundle of engine system pointers passed to the game DLL.
// The DLL must NOT create its own instances of these — it uses the host's.
struct EngineServices
{
    world*               game_world;
    IRenderAPI*          render_api;
    InputManager*        input_manager;
    ReflectionRegistry*  reflection;
    Application*         application;
    LevelManager*        level_manager;
    uint32_t             api_version;
};

// Platform export macro for game DLLs
#if defined(_WIN32)
#   define GAME_API extern "C" __declspec(dllexport)
#else
#   define GAME_API extern "C" __attribute__((visibility("default")))
#endif

// ============================================================================
// Required exports from every game DLL (client):
//
//   GAME_API int32_t     gardenGetAPIVersion();
//   GAME_API const char* gardenGetGameName();
//   GAME_API bool        gardenGameInit(EngineServices* services);
//   GAME_API void        gardenGameShutdown();
//   GAME_API void        gardenRegisterComponents(ReflectionRegistry* registry);
//   GAME_API void        gardenGameUpdate(float delta_time);
//   GAME_API void        gardenOnLevelLoaded();
//   GAME_API void        gardenOnPlayStart();
//   GAME_API void        gardenOnPlayStop();
//
// Optional server exports (resolved at runtime — absent = server not supported):
//
//   GAME_API bool        gardenServerInit(EngineServices* services);
//   GAME_API void        gardenServerShutdown();
//   GAME_API void        gardenServerUpdate(float delta_time);
//   GAME_API void        gardenServerOnLevelLoaded();
//   GAME_API void        gardenServerOnClientConnected(uint16_t client_id);
//   GAME_API void        gardenServerOnClientDisconnected(uint16_t client_id);
//
// ============================================================================
