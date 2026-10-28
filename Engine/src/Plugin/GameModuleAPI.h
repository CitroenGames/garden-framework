#pragma once

#include "Input/GameInputModeService.hpp"
#include "ECS/ComponentCatalog.hpp"

#include <cstddef>

#include <cstdint>

// Game modules pass STL-heavy engine objects across the DLL boundary
// (world, entt::registry, shared component storage). The compatibility token
// therefore includes both the source API revision and the C++ runtime/STL ABI
// shape that changes between Debug/Release builds on MSVC.
#define GARDEN_MODULE_API_REVISION 9

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
class InputActionSystem;
class ReflectionRegistry;
class Application;
class LevelManager;

// Synchronous registration input. The host owns identity and copies key bytes
// during submit; the module must not retain the sink or use it asynchronously.
struct ModuleComponentDescriptor {
    const char* key_utf8;
    size_t key_length;
    uint32_t revision;
    size_t size;
    size_t alignment;
    uint32_t capabilities;
    garden::ecs::ComponentOperations operations;
};
struct ModuleComponentSink {
    void* context;
    bool (*submit)(void*, const ModuleComponentDescriptor*) noexcept;
};

// Bundle of engine system pointers passed to the game DLL.
// The DLL must NOT create its own instances of these — it uses the host's.
struct EngineServices
{
    world*               game_world;
    IRenderAPI*          render_api;
    InputManager*        input_manager;
    InputActionSystem*   input_actions;
    ReflectionRegistry*  reflection;
    Application*         application;
    LevelManager*        level_manager;
    // Optional generic client-host input service.  It is deliberately null
    // for server and unsupported editor hosts.
    IGameInputModeService* input_mode_service = nullptr;
    uint32_t             api_version;

    // Network PIE fields (added in API version 3).
    // nullptr / 0 means "use default" for backward compatibility.
    const char*          connect_address = nullptr;  // server address to connect to
    uint16_t             connect_port    = 0;        // server port to connect to
    uint16_t             listen_port     = 0;        // port for server to listen on

    // FNV-1a hash of the exact level file loaded by the host (0 = no level or
    // unavailable source). Added in API revision 5 for the v3 network handshake.
    uint32_t             level_hash      = 0;

    // Normalized project-relative source or packaged level artifact selected by
    // the host. Host-owned storage remains valid until module shutdown. A null
    // value means the host has no selected on-disk level (for example, an
    // unsaved editor snapshot).
    const char*          selected_level_path = nullptr;
};

// Platform export macro for game DLLs
#if defined(_WIN32)
#   define GAME_API extern "C" __declspec(dllexport)
#else
#   define GAME_API extern "C" __attribute__((visibility("default")))
#endif

// ============================================================================
// Required core client exports from every game DLL:
//
//   GAME_API int32_t     gardenGetAPIVersion();
//   GAME_API const char* gardenGetGameName();
//   GAME_API bool        gardenGameInit(EngineServices* services);
//   GAME_API void        gardenGameShutdown();
//   GAME_API void        gardenRegisterComponents(ReflectionRegistry* registry);
//   GAME_API bool        gardenRegisterEcsComponents(ModuleComponentSink* sink);
// The ECS export must catch its own exceptions and return false on failure.
//   GAME_API void        gardenGameUpdate(float delta_time);
//
// Init returning false must synchronously roll back every thread, callback and
// resource created by that init; its matching shutdown export is not called.
// Successful role shutdown must join all module-created threads before return.
// The host separately drains host-owned jobs and callbacks before unmapping.
//
// EventBus access from a module is supported only on the host thread during
// an exported module callback. DLL static initialization/destruction and
// module-created worker threads must not access EventBus. Direct module use
// of sink/publish/queue/emplace currently prevents image retirement because
// EnTT event-pool lifetime is not yet owned by the host. Non-templated
// flush() is permitted; flush<Event>() can create a pool and blocks retirement.
//
// Optional client lifecycle exports (resolved at runtime — absent = no-op):
//
//   GAME_API void        gardenOnLevelLoaded();
//   GAME_API void        gardenOnPlayStart();
//   GAME_API void        gardenOnPlayStop();
//
// Optional server exports (resolved at runtime — absent = server not supported):
//
//   GAME_API bool        gardenServerInit(EngineServices* services);
//   GAME_API void        gardenServerShutdown();
//   GAME_API void        gardenServerPrePhysics(float fixed_delta);
//   GAME_API void        gardenServerUpdate(float delta_time);
//   GAME_API void        gardenServerOnLevelLoaded();
//   GAME_API void        gardenServerOnClientConnected(uint16_t client_id);
//   GAME_API void        gardenServerOnClientDisconnected(uint16_t client_id);
//
// gardenServerPrePhysics is called once per consumed server fixed tick, before
// engine Physics. It is optional so modules without pre-physics work need not
// export it.
// ============================================================================
