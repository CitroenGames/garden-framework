#include "Plugin/GameModuleAPI.h"
#include "world.hpp"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {
world* client_world = nullptr;
entt::entity sentinel = entt::null;
std::thread::id owner_thread;
bool refused = false;
bool quit_sent = false;

void record(const char* message) noexcept
{
    try {
        const char* path = std::getenv("GARDEN_CLIENT_RETRY_RECORDER");
        if (path && *path) { std::ofstream stream(path, std::ios::app); stream << message << '\n'; }
    } catch (...) {}
}

bool fault(void*, LifecyclePhase phase) noexcept
{
    if (phase != LifecyclePhase::Constraints || !client_world) return false;
    if (!refused) {
        refused = true;
        record("fault.first");
        return true;
    }
    record(std::this_thread::get_id() == owner_thread ? "fault.retry.same.thread" : "fault.retry.wrong.thread");
    record(client_world->registry.valid(sentinel) ? "fault.retry.world.retained" : "fault.retry.world.lost");
    // Destroy the type-erased context entry while its provider DLL is loaded.
    const bool removed = client_world->registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
    record(removed ? "fault.hook.removed" : "fault.hook.remove.failed");
    return false;
}
}

GAME_API int32_t gardenGetAPIVersion() { return GARDEN_MODULE_API_VERSION; }
GAME_API const char* gardenGetGameName() { return "ClientRetryFixture"; }
GAME_API bool gardenRegisterEcsComponents(ModuleComponentSink*) { return true; }
GAME_API void gardenRegisterComponents(ReflectionRegistry*) {}
GAME_API bool gardenGameInit(EngineServices* services)
{
    if (!services || !services->game_world) return false;
    client_world = services->game_world;
    owner_thread = std::this_thread::get_id();
    // Test-only sentinel avoids module-scoped created events; it is not an API contract.
    sentinel = client_world->registry.create();
    client_world->registry.ctx().emplace<garden::ecs::testing::LifecycleFaultHook>(
        garden::ecs::testing::LifecycleFaultHook{nullptr, &fault});
    record("client.init");
    return true;
}
GAME_API void gardenGameShutdown() { record("client.shutdown"); }
GAME_API void gardenGameUpdate(float)
{
    record("client.update");
    if (quit_sent) return;
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    if (SDL_PushEvent(&event)) { quit_sent = true; record("client.quit.requested"); }
}
GAME_API void gardenOnLevelLoaded() { record("client.level.loaded"); }
GAME_API void gardenOnPlayStart() { record("client.play.start"); }
GAME_API void gardenOnPlayStop() { record("client.play.stop"); }
