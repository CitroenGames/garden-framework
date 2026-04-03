#pragma once

#include <cstdint>

#define GARDEN_MODULE_API_VERSION 1

// Forward declarations
class world;
class IRenderAPI;
class InputManager;
class ReflectionRegistry;

// Bundle of engine system pointers passed to the game DLL.
// The DLL must NOT create its own instances of these — it uses the host's.
struct EngineServices
{
    world*               game_world;
    IRenderAPI*          render_api;
    InputManager*        input_manager;
    ReflectionRegistry*  reflection;
    uint32_t             api_version;
};

// Platform export macro for game DLLs
#if defined(_WIN32)
#   define GAME_API extern "C" __declspec(dllexport)
#else
#   define GAME_API extern "C" __attribute__((visibility("default")))
#endif

// ============================================================================
// Required exports from every game DLL:
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
// ============================================================================
