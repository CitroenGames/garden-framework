#include "Plugin/GameModuleAPI.h"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameModeBase.hpp"
#include "Events/EventBus.hpp"

#include <memory>
#include <new>

#include <cstdlib>
#include <fstream>

namespace
{
    const char* recorderPrefix()
    {
#if defined(FIXTURE_GENERATION_A)
        return "A.";
#elif defined(FIXTURE_GENERATION_B)
        return "B.";
#else
        return "";
#endif
    }

    void record(const char* event_name)
    {
        const char* configured_path = std::getenv("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER");
        if (!configured_path || !*configured_path) return;
        std::ofstream recorder(configured_path, std::ios::app);
        if (recorder) recorder << recorderPrefix() << event_name << '\n';
    }

    struct FixtureEvent { int value = 0; };
    void fixtureEventCallback(const FixtureEvent&) { record("event.callback"); }

    constexpr int32_t fixtureApiVersion()
    {
#if defined(FIXTURE_ABI_MISMATCH)
        constexpr uint32_t token = static_cast<uint32_t>(GARDEN_MODULE_API_VERSION);
        return static_cast<int32_t>((token & ~(0xffu << 24)) |
            (((((token >> 24) & 0xffu) + 1u) & 0xffu) << 24));
#else
        return GARDEN_MODULE_API_VERSION;
#endif
    }
}

GAME_API int32_t gardenGetAPIVersion() {
    if (std::getenv("GARDEN_TEST_MODULE_METADATA_EVENT"))
        EventBus::get().sink<FixtureEvent>().connect<&fixtureEventCallback>();
    return fixtureApiVersion();
}
GAME_API const char* gardenGetGameName() { return std::getenv("GARDEN_TEST_INVALID_MODULE_NAME") ? "" : "GameModuleLifecycleFixture"; }
GAME_API bool gardenGameInit(EngineServices*)
{
    record("client.init");
    if (std::getenv("GARDEN_TEST_MODULE_EVENT"))
        EventBus::get().sink<FixtureEvent>().connect<&fixtureEventCallback>();
#if defined(FIXTURE_CLIENT_INIT_FAILS)
    // The failure path must clean its partially registered callback itself.
    record("client.init.rollback_callback");
    return false;
#else
    return true;
#endif
}
namespace {
    struct ModuleFixtureComponent { int value; };
    bool constructModuleComponent(void* storage) noexcept
    {
        ::new (storage) ModuleFixtureComponent{73};
        return true;
    }
    void destroyModuleComponent(void* storage) noexcept
    {
        static_cast<ModuleFixtureComponent*>(storage)->~ModuleFixtureComponent();
    }
}

GAME_API bool gardenRegisterEcsComponents(ModuleComponentSink* sink)
{
#if defined(FIXTURE_ECS_PROVIDER) || defined(FIXTURE_ECS_BAD_BATCH) || defined(FIXTURE_ECS_EXPORT_FALSE) || defined(FIXTURE_ECS_STICKY_FAILURE)
    constexpr char key[] = "tests.lifecycle.ModuleFixtureComponent";
    ModuleComponentDescriptor descriptor{key, sizeof(key) - 1, 1, sizeof(ModuleFixtureComponent),
        alignof(ModuleFixtureComponent), garden::ecs::ComponentDefaultConstruct,
        {constructModuleComponent, destroyModuleComponent, nullptr, nullptr}};
    if (!sink || !sink->submit(sink->context, &descriptor)) return false;
#if defined(FIXTURE_ECS_BAD_BATCH)
    // Duplicate follows a valid entry; the host must reject the whole batch.
    return sink->submit(sink->context, &descriptor);
#elif defined(FIXTURE_ECS_EXPORT_FALSE)
    return false;
#elif defined(FIXTURE_ECS_STICKY_FAILURE)
    descriptor.key_utf8 = nullptr;
    sink->submit(sink->context, &descriptor);
    return true; // Ignoring a failed submit must not publish the first descriptor.
#else
    return true;
#endif
#else
    (void)sink;
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
#if !defined(FIXTURE_OMIT_GAME_UPDATE)
GAME_API void gardenGameUpdate(float) { record("client.update"); }
#endif
GAME_API void gardenOnLevelLoaded() { record("client.level_loaded"); }
GAME_API void gardenOnPlayStart() { record("client.play_start"); }
GAME_API void gardenOnPlayStop() { record("client.play_stop"); }

#if defined(FIXTURE_SERVER_RECORDER) || defined(FIXTURE_SERVER_INIT_FAILS)
GAME_API bool gardenServerInit(EngineServices*)
{
    record("server.init");
#if defined(FIXTURE_SERVER_INIT_FAILS)
    record("server.init.rollback_callback");
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

// Test-only operations are obtained from the exact provider image by the host.
GAME_API bool fixtureConstruct(void* storage) noexcept { *static_cast<int*>(storage) = 73; return true; }
GAME_API void fixtureDestroy(void* storage) noexcept { *static_cast<int*>(storage) = 0; }

GAME_API void fixtureFireEvent() { EventBus::get().publish(FixtureEvent{1}); }
