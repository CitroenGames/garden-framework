#include "Plugin/GameModuleAPI.h"

#include "GameSimulation.hpp"
#include "InputManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include <memory>

static EngineServices* g_services = nullptr;
static std::shared_ptr<InputManager> g_input_manager;
static std::unique_ptr<GameSimulation> g_simulation;

GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}

GAME_API const char* gardenGetGameName()
{
    return "ThirdPerson";
}

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    g_input_manager = services && services->input_manager
        ? std::shared_ptr<InputManager>(services->input_manager, [](InputManager*) {})
        : nullptr;
    return true;
}

GAME_API void gardenGameShutdown()
{
    g_simulation.reset();
    g_input_manager.reset();
    g_services = nullptr;
}

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
}

GAME_API void gardenGameUpdate(float delta_time)
{
    if (!g_simulation)
        return;

    if (g_input_manager)
    {
        const float mouse_dx = g_input_manager->get_mouse_delta_x();
        const float mouse_dy = g_input_manager->get_mouse_delta_y();
        if (mouse_dx != 0.0f || mouse_dy != 0.0f)
            g_simulation->handleMouseMotion(mouse_dy, mouse_dx);
    }

    g_simulation->update(delta_time);
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services || !g_services->game_world)
        return;

    g_simulation = std::make_unique<GameSimulation>(g_services->game_world, g_input_manager);
    g_simulation->initialize();
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
