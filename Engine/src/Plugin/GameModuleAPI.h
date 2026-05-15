#pragma once

#include <cstdint>

// Game modules pass STL-heavy engine objects across the DLL boundary
// (world, entt::registry, shared component storage). The compatibility token
// therefore includes both the source API revision and the C++ runtime/STL ABI
// shape that changes between Debug/Release builds on MSVC.
#define GARDEN_MODULE_API_REVISION 4

#if defined(_MSC_VER)
#   define GARDEN_MODULE_COMPILER_FAMILY 1
#   define GARDEN_MODULE_COMPILER_VERSION (_MSC_VER)
#else
#   define GARDEN_MODULE_COMPILER_FAMILY 0
#   define GARDEN_MODULE_COMPILER_VERSION 0
#endif

#if defined(_ITERATOR_DEBUG_LEVEL)
#   define GARDEN_MODULE_STL_DEBUG_LEVEL (_ITERATOR_DEBUG_LEVEL)
#elif defined(_GLIBCXX_DEBUG) || (defined(_LIBCPP_DEBUG) && _LIBCPP_DEBUG)
#   define GARDEN_MODULE_STL_DEBUG_LEVEL 1
#else
#   define GARDEN_MODULE_STL_DEBUG_LEVEL 0
#endif

#if defined(_DEBUG)
#   define GARDEN_MODULE_DEBUG_RUNTIME 1
#else
#   define GARDEN_MODULE_DEBUG_RUNTIME 0
#endif

#define GARDEN_MODULE_API_VERSION (static_cast<int32_t>( \
    ((GARDEN_MODULE_API_REVISION & 0xff) << 24) | \
    ((GARDEN_MODULE_COMPILER_FAMILY & 0x0f) << 20) | \
    ((GARDEN_MODULE_COMPILER_VERSION & 0x0fff) << 8) | \
    ((GARDEN_MODULE_STL_DEBUG_LEVEL & 0x0f) << 4) | \
    (GARDEN_MODULE_DEBUG_RUNTIME & 0x0f)))

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

    // Network PIE fields (added in API version 3).
    // nullptr / 0 means "use default" for backward compatibility.
    const char*          connect_address = nullptr;  // server address to connect to
    uint16_t             connect_port    = 0;        // server port to connect to
    uint16_t             listen_port     = 0;        // port for server to listen on
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
