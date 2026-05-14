#include "Plugin/GameModuleAPI.h"

#include "Components/Components.hpp"
#include "InputManager.hpp"
#include "Utils/Log.hpp"
#include "XR/OpenXRSystem.hpp"
#include "world.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
    constexpr float kHeadOffsetY = 0.45f;
    constexpr float kMouseRadiansPerPixel = 0.001f;
    constexpr float kSnapTurnRadians = 0.5235987756f;
    constexpr float kMaxFrameDelta = 0.1f;

    EngineServices* g_services = nullptr;
    std::shared_ptr<InputManager> g_input_manager;
    entt::entity g_player_entity = entt::null;
    bool g_owns_openxr = false;
    float g_yaw = 0.0f;
    float g_pitch = 0.0f;

    float clampPitch(float pitch)
    {
        return std::clamp(pitch, -1.35f, 1.35f);
    }

    float degreesToRadians(float degrees)
    {
        return degrees * 0.01745329252f;
    }

    entt::entity findPlayer(world& game_world)
    {
        auto view = game_world.registry.view<PlayerComponent, TransformComponent>();
        for (entt::entity entity : view)
            return entity;
        return entt::null;
    }

    CharacterMoveInput collectMoveInput()
    {
        CharacterMoveInput input;
        if (!g_input_manager)
            return input;

        if (g_input_manager->is_key_held(SDL_SCANCODE_W)) input.move_forward += 1.0f;
        if (g_input_manager->is_key_held(SDL_SCANCODE_S)) input.move_forward -= 1.0f;
        if (g_input_manager->is_key_held(SDL_SCANCODE_A)) input.move_right += 1.0f;
        if (g_input_manager->is_key_held(SDL_SCANCODE_D)) input.move_right -= 1.0f;
        if (g_input_manager->is_key_pressed(SDL_SCANCODE_SPACE)) input.buttons |= CharacterMoveFlags::Jump;

        input.camera_yaw = g_yaw;
        input.camera_pitch = g_pitch;
        return input;
    }

    void updateLookInput()
    {
        if (!g_input_manager)
            return;

        g_pitch = clampPitch(g_pitch + g_input_manager->get_mouse_delta_y() *
            g_input_manager->Sensitivity_Y * kMouseRadiansPerPixel);
        g_yaw -= g_input_manager->get_mouse_delta_x() *
            g_input_manager->Sensitivity_X * kMouseRadiansPerPixel;

        if (g_input_manager->is_key_pressed(SDL_SCANCODE_Q))
            g_yaw += kSnapTurnRadians;
        if (g_input_manager->is_key_pressed(SDL_SCANCODE_E))
            g_yaw -= kSnapTurnRadians;
    }

    void updateCameraFromBody(const CharacterControllerState& state)
    {
        if (!g_services || !g_services->game_world)
            return;

        camera& cam = g_services->game_world->world_camera;
        cam.position = state.position + glm::vec3(0.0f, kHeadOffsetY, 0.0f);
        cam.rotation = glm::vec3(g_pitch, g_yaw, 0.0f);
    }

    void initializeOpenXR()
    {
        XR::OpenXRSystem& xr = XR::OpenXRSystem::get();
        if (xr.isInitialized())
            return;

        XR::OpenXRInitDesc desc;
        desc.application_name = "Garden VR Template";
        g_owns_openxr = xr.initialize(desc) && xr.isInitialized();

        const XR::OpenXRStatus& status = xr.getStatus();
        if (!status.last_error.empty())
            LOG_ENGINE_WARN("[VRTemplate] OpenXR runtime probe: {}", status.last_error);
    }
}

GAME_API int32_t gardenGetAPIVersion()
{
    return GARDEN_MODULE_API_VERSION;
}

GAME_API const char* gardenGetGameName()
{
    return "VR";
}

GAME_API bool gardenGameInit(EngineServices* services)
{
    g_services = services;
    g_input_manager = services && services->input_manager
        ? std::shared_ptr<InputManager>(services->input_manager, [](InputManager*) {})
        : nullptr;
    initializeOpenXR();
    return true;
}

GAME_API void gardenGameShutdown()
{
    if (g_owns_openxr && XR::OpenXRSystem::get().isInitialized())
        XR::OpenXRSystem::get().shutdown();

    g_owns_openxr = false;
    g_player_entity = entt::null;
    g_input_manager.reset();
    g_services = nullptr;
}

GAME_API void gardenRegisterComponents(ReflectionRegistry* registry)
{
    (void)registry;
}

GAME_API void gardenGameUpdate(float delta_time)
{
    if (!g_services || !g_services->game_world || g_player_entity == entt::null)
        return;

    XR::OpenXRSystem& xr = XR::OpenXRSystem::get();
    if (xr.isInitialized())
        xr.pollEvents();

    world& game_world = *g_services->game_world;
    if (!game_world.registry.valid(g_player_entity))
        return;

    updateLookInput();

    const float clamped_delta = std::clamp(delta_time, 0.0f, kMaxFrameDelta);
    game_world.step_physics(clamped_delta);

    CharacterControllerState state = game_world.simulate_character_controller(
        g_player_entity, collectMoveInput(), clamped_delta);
    game_world.player_collisions(g_player_entity);

    updateCameraFromBody(state);
}

GAME_API void gardenOnLevelLoaded()
{
    if (!g_services || !g_services->game_world)
        return;

    world& game_world = *g_services->game_world;
    g_player_entity = findPlayer(game_world);
    if (g_player_entity == entt::null)
    {
        LOG_ENGINE_WARN("[VRTemplate] No Player entity found in the loaded level");
        return;
    }

    auto& transform = game_world.registry.get<TransformComponent>(g_player_entity);
    const bool spawned_at_camera =
        glm::distance(game_world.world_camera.position, transform.position) < 0.01f;

    if (spawned_at_camera)
        transform.position.y -= kHeadOffsetY;

    if (!game_world.getPhysicsSystem().hasCharacterController(g_player_entity))
        game_world.create_character_controller(g_player_entity);

    if (spawned_at_camera)
        game_world.teleport_character_controller(g_player_entity, transform.position);

    g_pitch = spawned_at_camera
        ? clampPitch(game_world.world_camera.rotation.x)
        : clampPitch(degreesToRadians(transform.rotation.x));
    g_yaw = spawned_at_camera
        ? game_world.world_camera.rotation.y
        : degreesToRadians(transform.rotation.y);

    CharacterControllerState state = game_world.get_character_controller_state(g_player_entity);
    updateCameraFromBody(state);
}

GAME_API void gardenOnPlayStart()
{
}

GAME_API void gardenOnPlayStop()
{
}
