#include "Plugin/GameModuleAPI.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>

namespace
{
    void record(const char* event_name)
    {
        const char* configured_path = std::getenv("GARDEN_GAME_MODULE_CONTRACT_RECORDER");
        if (!configured_path || !*configured_path) return;
        std::ofstream recorder(configured_path, std::ios::app);
        if (recorder) recorder << event_name << '\n';
    }

    constexpr int32_t fixtureApiVersion()
    {
        constexpr uint32_t host_version = static_cast<uint32_t>(GARDEN_MODULE_API_VERSION);
#if defined(FIXTURE_ABI_REVISION_MISMATCH)
        return static_cast<int32_t>((host_version & ~(0xffu << 24)) |
                                    (((((host_version >> 24) & 0xffu) + 1u) & 0xffu) << 24));
#elif defined(FIXTURE_ABI_COMPILER_FAMILY_MISMATCH)
        return static_cast<int32_t>((host_version & ~(0x0fu << 20)) |
                                    (((((host_version >> 20) & 0x0fu) + 1u) & 0x0fu) << 20));
#elif defined(FIXTURE_ABI_COMPILER_VERSION_MISMATCH)
        return static_cast<int32_t>((host_version & ~(0x0fffu << 8)) |
                                    (((((host_version >> 8) & 0x0fffu) + 1u) & 0x0fffu) << 8));
#elif defined(FIXTURE_ABI_STL_DEBUG_LEVEL_MISMATCH)
        return static_cast<int32_t>((host_version & ~(0x0fu << 4)) |
                                    (((((host_version >> 4) & 0x0fu) + 1u) & 0x0fu) << 4));
#elif defined(FIXTURE_ABI_DEBUG_RUNTIME_MISMATCH)
        return static_cast<int32_t>((host_version & ~0x0fu) |
                                    ((((host_version & 0x0fu) + 1u) & 0x0fu)));
#else
        return GARDEN_MODULE_API_VERSION;
#endif
    }
}

#ifndef FIXTURE_OMIT_GET_API_VERSION
GAME_API int32_t gardenGetAPIVersion() { return fixtureApiVersion(); }
#endif

#ifndef FIXTURE_OMIT_GET_GAME_NAME
GAME_API const char* gardenGetGameName() { return "GameModuleContractFixture"; }
#endif

#ifndef FIXTURE_OMIT_GAME_INIT
GAME_API bool gardenGameInit(EngineServices*) { record("client.init"); return true; }
#endif

#ifndef FIXTURE_OMIT_GAME_SHUTDOWN
GAME_API void gardenGameShutdown() { record("client.shutdown"); }
#endif

#ifndef FIXTURE_OMIT_REGISTER_COMPONENTS
GAME_API void gardenRegisterComponents(ReflectionRegistry*) { record("client.register_components"); }
#endif

#ifndef FIXTURE_OMIT_GAME_UPDATE
GAME_API void gardenGameUpdate(float) { record("client.update"); }
#endif

#if defined(FIXTURE_CLIENT_LIFECYCLE_RECORDER)
GAME_API void gardenOnLevelLoaded() { record("client.level_loaded"); }
GAME_API void gardenOnPlayStart() { record("client.play_start"); }
GAME_API void gardenOnPlayStop() { record("client.play_stop"); }
#endif

#if defined(FIXTURE_SERVER_INIT) || defined(FIXTURE_SERVER_PARTIAL) || defined(FIXTURE_SERVER_COMPLETE)
GAME_API bool gardenServerInit(EngineServices*) { record("server.init"); return true; }
#endif

#if defined(FIXTURE_SERVER_PARTIAL) || defined(FIXTURE_SERVER_COMPLETE) || defined(FIXTURE_SERVER_CALLBACKS_WITHOUT_INIT)
GAME_API void gardenServerUpdate(float) { record("server.update"); }
GAME_API void gardenServerOnClientConnected(uint16_t) { record("server.client_connected"); }
#endif

#if defined(FIXTURE_SERVER_COMPLETE)
GAME_API void gardenServerShutdown() { record("server.shutdown"); }
GAME_API void gardenServerPrePhysics(float) { record("server.pre_physics"); }
GAME_API void gardenServerOnLevelLoaded() { record("server.level_loaded"); }
GAME_API void gardenServerOnClientDisconnected(uint16_t) { record("server.client_disconnected"); }
#endif
