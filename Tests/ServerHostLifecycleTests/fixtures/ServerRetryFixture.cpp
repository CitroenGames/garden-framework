#include "Plugin/GameModuleAPI.h"
#include "world.hpp"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <fstream>

namespace {
world* server_world = nullptr;
bool quit_sent = false;

void record(const char* message)
{
    const char* path = std::getenv("GARDEN_SERVER_RETRY_RECORDER");
    if (!path || !*path) return;
    std::ofstream stream(path, std::ios::app);
    if (stream) stream << message << '\n';
}

bool failFirstShutdownConstraints(void*, LifecyclePhase phase) noexcept
{
    if (phase != LifecyclePhase::Constraints || !server_world) return false;
    record("fault.constraints");
    // The context's type-erased deleter was instantiated in this DLL. Remove
    // the entire entry while the DLL is still loaded, not just its callback.
    const bool removed = server_world->registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
    record(removed ? "fault.hook.removed" : "fault.hook.remove.failed");
    return true;
}

bool failFirstSceneRetirement(void*, LifecyclePhase phase) noexcept
{
    if (phase != LifecyclePhase::Hierarchy || !server_world) return false;
    record("scene.fault.hierarchy");
    const bool removed = server_world->registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
    record(removed ? "scene.hook.removed" : "scene.hook.remove.failed");
    return true;
}
}

GAME_API int32_t gardenGetAPIVersion() { return GARDEN_MODULE_API_VERSION; }
GAME_API const char* gardenGetGameName() { return "ServerRetryFixture"; }
GAME_API bool gardenRegisterEcsComponents(ModuleComponentSink*) { return true; }
GAME_API void gardenRegisterComponents(ReflectionRegistry*) {}
GAME_API bool gardenGameInit(EngineServices*) { return true; }
GAME_API void gardenGameShutdown() {}
GAME_API void gardenGameUpdate(float) {}
GAME_API void gardenOnLevelLoaded() {}
GAME_API void gardenOnPlayStart() {}
GAME_API void gardenOnPlayStop() {}

GAME_API bool gardenServerInit(EngineServices* services)
{
    if (!services || !services->game_world) return false;
    server_world = services->game_world;
    // This entity is outside the SceneManager cohort and exercises creation
    // from a module callback before world shutdown retires it.
    const auto created = server_world->createEntity("module-created");
    if (created.status != ScopedRestoreStatus::Completed || created.rollback.operation.valid()) {
        record("server.entity.create.failed");
        return false;
    }
    record("server.entity.created");
    server_world->registry.ctx().emplace<garden::ecs::testing::LifecycleFaultHook>(
        garden::ecs::testing::LifecycleFaultHook{nullptr, &failFirstSceneRetirement});
    record("server.init");
    return true;
}

GAME_API void gardenServerOnLevelLoaded() {}
GAME_API void gardenServerPrePhysics(float) {}
GAME_API void gardenServerUpdate(float)
{
    if (quit_sent) return;
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    if (SDL_PushEvent(&event)) {
        quit_sent = true;
        record("server.quit.requested");
    }
}
GAME_API void gardenServerShutdown()
{
    record("server.shutdown");
    if (server_world) {
        // An empty batch is a non-mutating probe: it is refused if the scene's
        // lifecycle operation is still outstanding or unacknowledged.
        const auto probe = server_world->retireEntities({});
        record(probe.cleanupComplete() ? "scene.ack.observed" : "scene.ack.missing");
        server_world->registry.ctx().insert_or_assign(
            garden::ecs::testing::LifecycleFaultHook{nullptr, &failFirstShutdownConstraints});
    }
}
