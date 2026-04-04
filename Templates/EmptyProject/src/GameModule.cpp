#include "Plugin/GameModuleAPI.h"
#include "Reflection/Reflect.hpp"
#include "Reflection/ReflectionRegistry.hpp"

static EngineServices* g_services = nullptr;

GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}

GAME_API const char* gardenGetGameName()
{
    return "EmptyProject";
}

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    return true;
}

GAME_API void gardenGameShutdown()
{
    g_services = nullptr;
}

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
    // Register your game components here:
    // registerReflection_MyComponent(*registry);
}

GAME_API void gardenGameUpdate(float delta_time)
{
    // Game logic per frame
}

GAME_API void gardenOnLevelLoaded()
{
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
