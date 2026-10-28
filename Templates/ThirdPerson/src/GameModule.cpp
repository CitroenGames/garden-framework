#include "Plugin/GameModuleAPI.h"

#include "GameSimulation.hpp"
#include "InputManager.hpp"
#include "InputActionSystem.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <memory>
#include <string>

static EngineServices* g_services = nullptr;
static std::shared_ptr<InputManager> g_input_manager;
static std::unique_ptr<GameSimulation> g_simulation;
static const LocalPlayerInputActionNames g_local_input_actions = {
    "ThirdPerson.Move", "ThirdPerson.Jump", "ThirdPerson.Sprint",
    "ThirdPerson.Crouch", "ThirdPerson.FreecamVertical", "ThirdPerson.FreecamFast"
};
static const std::string g_local_input_context = "ThirdPerson.Gameplay";

static bool configureThirdPersonInput(InputActionSystem& actions)
{
    const std::array<InputActionDef, 6> definitions = {{
        { g_local_input_actions.move, ActionValueType::Axis2D, false },
        { g_local_input_actions.jump, ActionValueType::Button, false },
        { g_local_input_actions.sprint, ActionValueType::Button, false },
        { g_local_input_actions.crouch, ActionValueType::Button, false },
        { g_local_input_actions.freecam_vertical, ActionValueType::Axis1D, false },
        { g_local_input_actions.freecam_fast, ActionValueType::Button, false }
    }};
    for (const InputActionDef& definition : definitions)
    {
        if (!actions.registerAction(definition))
            return false;
    }

    MappingContext gameplay;
    gameplay.name = g_local_input_context;
    auto key = [&gameplay](const std::string& action, SDL_Scancode code,
                           bool invert = false, uint8_t axis_target = 0) {
        gameplay.bindings.push_back({
            action, InputBinding::Source::Key, code, 1.0f, invert, axis_target });
    };
    key(g_local_input_actions.move, SDL_SCANCODE_W, false, 1);
    key(g_local_input_actions.move, SDL_SCANCODE_S, true, 1);
    key(g_local_input_actions.move, SDL_SCANCODE_A, true, 0);
    key(g_local_input_actions.move, SDL_SCANCODE_D, false, 0);
    key(g_local_input_actions.jump, SDL_SCANCODE_SPACE);
    key(g_local_input_actions.sprint, SDL_SCANCODE_LSHIFT);
    key(g_local_input_actions.crouch, SDL_SCANCODE_LCTRL);
    key(g_local_input_actions.crouch, SDL_SCANCODE_C);
    key(g_local_input_actions.freecam_vertical, SDL_SCANCODE_SPACE);
    key(g_local_input_actions.freecam_vertical, SDL_SCANCODE_LSHIFT, true);
    key(g_local_input_actions.freecam_fast, SDL_SCANCODE_LSHIFT);
    return actions.pushContext(gameplay);
}

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
    if (!services || !services->input_actions ||
        !configureThirdPersonInput(*services->input_actions))
        return false;
    g_services = services;
    g_input_manager = services && services->input_manager
        ? std::shared_ptr<InputManager>(services->input_manager, [](InputManager*) {})
        : nullptr;
    return true;
}

GAME_API void gardenGameShutdown()
{
    g_simulation.reset();
    if (g_services && g_services->input_actions)
        g_services->input_actions->popContext(g_local_input_context.c_str());
    g_input_manager.reset();
    g_services = nullptr;
}

GAME_API bool gardenRegisterEcsComponents(ModuleComponentSink*)
{
    return true;
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

    g_simulation = std::make_unique<GameSimulation>(
        g_services->game_world, g_input_manager, g_services->input_actions,
        g_local_input_actions, std::string{});
    g_simulation->initialize();
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
