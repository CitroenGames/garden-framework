#include "Plugin/GameModuleAPI.h"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameModeBase.hpp"

#include <memory>

#include <cstdlib>
#include <fstream>

namespace
{
    void record(const char* event_name)
    {
        const char* configured_path = std::getenv("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER");
        if (!configured_path || !*configured_path) return;
        std::ofstream recorder(configured_path, std::ios::app);
        if (recorder) recorder << event_name << '\n';
    }
}

GAME_API int32_t gardenGetAPIVersion() { return GARDEN_MODULE_API_VERSION; }
GAME_API const char* gardenGetGameName() { return "GameModuleLifecycleFixture"; }
GAME_API bool gardenGameInit(EngineServices*)
{
    record("client.init");
#if defined(FIXTURE_CLIENT_INIT_FAILS)
    return false;
#else
    return true;
#endif
}
GAME_API void gardenGameShutdown() { record("client.shutdown"); }
GAME_API void gardenRegisterComponents(ReflectionRegistry*)
{
    record("register_components");
#if defined(FIXTURE_OWNED_TEARDOWN)
    // This factory closes over a function emitted by this DLL. The host must
    // remove it by its explicit game-name source before unloading the image.
    GameFramework::GameModeRegistry::get().registerGameMode(
        "GameModuleLifecycleOwnedFactory",
        []() { return std::make_unique<GameFramework::GameModeBase>(); },
        "GameModuleLifecycleFixture");
    record("factory.registered");
#endif
}
GAME_API void gardenGameUpdate(float) { record("client.update"); }
GAME_API void gardenOnLevelLoaded() { record("client.level_loaded"); }
GAME_API void gardenOnPlayStart() { record("client.play_start"); }
GAME_API void gardenOnPlayStop() { record("client.play_stop"); }

#if defined(FIXTURE_SERVER_RECORDER) || defined(FIXTURE_SERVER_INIT_FAILS)
GAME_API bool gardenServerInit(EngineServices*)
{
    record("server.init");
#if defined(FIXTURE_SERVER_INIT_FAILS)
    return false;
#else
    return true;
#endif
}
GAME_API void gardenServerShutdown() { record("server.shutdown"); }
GAME_API void gardenServerPrePhysics(float) { record("server.pre_physics"); }
GAME_API void gardenServerUpdate(float) { record("server.update"); }
GAME_API void gardenServerOnLevelLoaded() { record("server.level_loaded"); }
#endif
