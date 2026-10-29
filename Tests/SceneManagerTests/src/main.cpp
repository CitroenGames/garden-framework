#include "Graphics/HeadlessRenderAPI.hpp"
#include "Scene/SceneManager.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "Components/IdentityComponent.hpp"
#include "world.hpp"
#include "Utils/Log.hpp"
#include "spdlog/sinks/callback_sink.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>

namespace garden::testing { ENGINE_API void failNextScenePublicationForTesting() noexcept; }

namespace
{
int g_failures = 0;
int g_completion_count = 0;
bool g_last_completion_success = false;
std::thread::id g_completion_thread;

void recordCompletion(const SceneLoadCompleteEvent& event)
{
    ++g_completion_count;
    g_last_completion_success = event.success;
    g_completion_thread = std::this_thread::get_id();
}

void check(bool condition, const std::string& name, const std::string& reason)
{
    if (condition)
        std::cout << "[PASS] " << name << std::endl;
    else
    {
        std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
        ++g_failures;
    }
}

bool completed(const SceneResult& result, const std::string& name, const char* action)
{
    const bool ok = (result.status == SceneStatus::Completed ||
                     result.status == SceneStatus::CompletedWithObserverFailure) && result.cleanup_complete;
    check(ok, name, action);
    return ok;
}

struct TempLevel
{
    std::filesystem::path path;

    explicit TempLevel(const std::string& name)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("garden_scene_manager_" + name + "_" + std::to_string(stamp) + ".level.json");
    }

    ~TempLevel()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    bool write(const std::string& contents)
    {
        std::ofstream file(path, std::ios::binary);
        file << contents;
        return file.good();
    }
};

bool waitForLoad(SceneManager& manager, SceneId id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto pump = manager.pumpAsyncLoads();
        if ((pump.status != SceneStatus::Completed && pump.status != SceneStatus::CompletedWithObserverFailure) || !pump.cleanup_complete)
            return false;
        const SceneLoadState state = manager.getSceneState(id);
        if (state == SceneLoadState::Loaded || state == SceneLoadState::Failed)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void resetCompletionEvents()
{
    EventBus::get().sink<SceneLoadCompleteEvent>().disconnect<&recordCompletion>();
    EventBus::get().clear();
    g_completion_count = 0;
    g_last_completion_success = false;
    g_completion_thread = std::thread::id{};
}

void testAsyncLoadSuccessAndFailure()
{
    const std::string name = "async load commits success and failure on owner thread";
    resetCompletionEvents();
    EventBus::get().sink<SceneLoadCompleteEvent>().connect<&recordCompletion>();

    TempLevel valid("success");
    if (!valid.write(R"({"metadata":{"level_name":"async-test"},"entities":[]})"))
    {
        check(false, name, "could not write level fixture");
        return;
    }

    const auto owner_thread = std::this_thread::get_id();
    world game_world;
    SceneManager manager;
    completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
    auto success_load = manager.loadSceneAsync(valid.path.string());
    completed(success_load, name, "async load failed");
    const SceneId success_id = success_load.scene_id;
    check(success_id != INVALID_SCENE, name, "async load did not allocate a scene id");
    check(manager.getSceneState(success_id) == SceneLoadState::Loading,
          name, "worker changed scene state before owner-thread pump");
    check(waitForLoad(manager, success_id), name, "success load did not complete");
    check(manager.getSceneState(success_id) == SceneLoadState::Loaded,
          name, "successful parse did not become Loaded");
    check(manager.getLoadProgress(success_id) == 1.0f,
          name, "successful parse did not reach full progress");
    check(g_completion_count == 1 && g_last_completion_success,
          name, "success completion event was not published exactly once");
    check(g_completion_thread == owner_thread,
          name, "completion event ran on the worker instead of the owner thread");

    TempLevel missing("missing");
    auto failed_load = manager.loadSceneAsync(missing.path.string());
    completed(failed_load, name, "missing async load did not start");
    const SceneId failed_id = failed_load.scene_id;
    check(waitForLoad(manager, failed_id), name, "missing-file load did not complete");
    check(manager.getSceneState(failed_id) == SceneLoadState::Failed,
          name, "missing file did not produce Failed state");
    check(g_completion_count == 2 && !g_last_completion_success,
          name, "failure completion event was not published on the owner thread");
    check(g_completion_thread == owner_thread,
          name, "failure completion event ran on the worker thread");
    completed(manager.shutdown(), name, "shutdown failed");
    resetCompletionEvents();
}

void testCancelAndClearPendingLoads()
{
    const std::string name = "unload and clear cancel pending loads";
    TempLevel valid("cancel");
    if (!valid.write(R"({"metadata":{"level_name":"cancel-test"},"entities":[]})"))
    {
        check(false, name, "could not write level fixture");
        return;
    }

    world game_world;
    SceneManager manager;
    completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
    auto unloaded_load = manager.loadSceneAsync(valid.path.string());
    completed(unloaded_load, name, "async load failed");
    const SceneId unloaded_id = unloaded_load.scene_id;
    completed(manager.unloadScene(unloaded_id), name, "unload failed");
    check(manager.getSceneState(unloaded_id) == SceneLoadState::Unloaded,
          name, "unloaded async scene remained registered");
    auto unloaded_pump = manager.pumpAsyncLoads();
    check(completed(unloaded_pump, name, "pump failed") && unloaded_pump.completed_count == 0,
          name, "unloaded scene produced a late completion");

    auto cleared_load = manager.loadSceneAsync(valid.path.string());
    completed(cleared_load, name, "second async load failed");
    const SceneId cleared_id = cleared_load.scene_id;
    completed(manager.clear(), name, "clear failed");
    check(manager.getSceneState(cleared_id) == SceneLoadState::Unloaded,
          name, "clear retained a pending scene");
    auto cleared_pump = manager.pumpAsyncLoads();
    check(completed(cleared_pump, name, "pump after clear failed") && cleared_pump.completed_count == 0,
          name, "clear left a worker result available for late commit");
    completed(manager.shutdown(), name, "shutdown failed");
}

void testShutdownDuringLoad()
{
    const std::string name = "shutdown joins its loading worker";
    resetCompletionEvents();
    EventBus::get().sink<SceneLoadCompleteEvent>().connect<&recordCompletion>();
    TempLevel valid("manager-lifetime");
    if (!valid.write(R"({"metadata":{"level_name":"lifetime-test"},"entities":[]})"))
    {
        check(false, name, "could not write level fixture");
        return;
    }

    {
        world game_world;
        SceneManager manager;
        completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
        auto load = manager.loadSceneAsync(valid.path.string());
        check(completed(load, name, "async load failed") && load.scene_id != INVALID_SCENE,
              name, "could not start async load");
        completed(manager.shutdown(), name, "shutdown did not join worker");
    }
    check(g_completion_count == 0, name,
          "worker published a completion event without owner-thread pumping");
    resetCompletionEvents();
}

void testOperationIdentityAndHeadless() {
    const std::string name = "operation handles reject stale and foreign managers";
    TempLevel level("operation");
    if (!level.write(R"({"metadata":{"level_name":"operation"},"entities":[]})")) {
        check(false, name, "could not write fixture"); return;
    }
    world game_world;
    SceneManager first, second;
    completed(first.initialize(&game_world, nullptr), name, "headless initialization failed");
    completed(second.initialize(&game_world, nullptr), name, "second initialization failed");
    auto loaded = first.loadScene(level.path.string());
    check(loaded.status == SceneStatus::Completed && loaded.cleanup_complete && !loaded.operation_handle.valid(), name, "load result incorrect");
    auto activated = first.activateScene(loaded.scene_id);
    check(activated.status == SceneStatus::Completed && activated.cleanup_complete && activated.operation_handle.valid(), name, "headless activation failed");
    auto stale = first.resume(activated.operation_handle);
    auto foreign = second.resume(activated.operation_handle);
    check(stale.status == SceneStatus::InvalidArgument && stale.cleanup_complete, name, "terminal handle resumed");
    check(foreign.status == SceneStatus::InvalidArgument && foreign.cleanup_complete, name, "foreign handle resumed");
    completed(first.shutdown(), name, "shutdown failed");
    completed(second.shutdown(), name, "second shutdown failed");
}

struct FailLifecycleOnce {
    LifecyclePhase phase = LifecyclePhase::Constraints;
    bool armed = true;
};
bool failLifecycleOnce(void* context, LifecyclePhase phase) noexcept {
    auto& fault = *static_cast<FailLifecycleOnce*>(context);
    if (fault.armed && phase == fault.phase) { fault.armed = false; return true; }
    return false;
}
int g_level_loaded_count = 0;
int g_unload_attempts = 0;
void recordLevelLoaded(const LevelLoadedEvent&) { ++g_level_loaded_count; }
void throwOnUnload(const SceneUnloadEvent&) { ++g_unload_attempts; throw std::runtime_error("injected observer failure"); }

void testPublicationFailureAndRetry() {
    const std::string name = "publication staging failure retains cohort through partial retirement";
    TempLevel level("publication-fault");
    if (!level.write(R"({"metadata":{"level_name":"publication"},"entities":[{"name":"owned","type":"Static"}]})")) {
        check(false, name, "could not write fixture"); return;
    }
    world game_world;
    SceneManager manager;
    completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
    auto loaded = manager.loadScene(level.path.string());
    completed(loaded, name, "load failed");
    g_level_loaded_count = 0;
    EventBus::get().sink<LevelLoadedEvent>().connect<&recordLevelLoaded>();
    FailLifecycleOnce fault;
    game_world.registry.ctx().emplace<garden::ecs::testing::LifecycleFaultHook>(
        garden::ecs::testing::LifecycleFaultHook{&fault, &failLifecycleOnce});
    garden::testing::failNextScenePublicationForTesting();
    auto partial = manager.activateScene(loaded.scene_id);
    check(!fault.armed && partial.status == SceneStatus::LifecycleFailure && !partial.cleanup_complete &&
          partial.operation_handle.valid() && partial.lifecycle.operation.valid(), name, "staging failure did not retain a partial retirement");
    check(partial.construction.status == LevelInstantiationStatus::AllocationFailure &&
          partial.construction.entities.size() == 1, name, "construction identity or cause was lost");
    check(manager.getSceneState(loaded.scene_id) == SceneLoadState::Loaded && !manager.getActiveScene() &&
          g_level_loaded_count == 0, name, "failed staging published a scene");
    auto busy = manager.clear();
    check(busy.status == SceneStatus::Busy && busy.operation_handle == partial.operation_handle &&
          !busy.cleanup_complete, name, "other mutation was not blocked");
    auto lifecycle_token = partial.lifecycle.operation;
    auto handles = partial.construction.entities;
    auto done = manager.resume(partial.operation_handle);
    check(done.status == SceneStatus::ConstructionFailure && done.cleanup_complete &&
          done.construction.status == LevelInstantiationStatus::AllocationFailure, name, "resume did not finish staging cleanup");
    check(game_world.retirementResult(lifecycle_token).status == LifecycleStatus::UnknownOperation,
          name, "completed retirement was not acknowledged");
    for (auto handle : handles)
        check(game_world.classifyEntityHandle(handle) == EntityHandleClassification::RetiredGone,
              name, "captured incarnation survived cleanup");
    auto stale = manager.resume(partial.operation_handle);
    check(stale.status == SceneStatus::InvalidArgument && stale.cleanup_complete, name, "completed scene operation resumed");
    game_world.registry.ctx().erase<garden::ecs::testing::LifecycleFaultHook>();
    auto activated = manager.activateScene(loaded.scene_id);
    check(completed(activated, name, "retry activation failed") && g_level_loaded_count == 1,
          name, "retry did not publish exactly once");
    EventBus::get().sink<LevelLoadedEvent>().disconnect<&recordLevelLoaded>();
    completed(manager.shutdown(), name, "shutdown failed");
}

void testThrowingUnloadObserver() {
    const std::string name = "throwing unload observer is attempted once";
    TempLevel level("observer");
    if (!level.write(R"({"metadata":{"level_name":"observer"},"entities":[]})")) {
        check(false, name, "could not write fixture"); return;
    }
    world game_world;
    SceneManager manager;
    completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
    auto loaded = manager.loadScene(level.path.string());
    completed(loaded, name, "load failed");
    completed(manager.activateScene(loaded.scene_id), name, "activate failed");
    g_unload_attempts = 0;
    EventBus::get().sink<SceneUnloadEvent>().connect<&throwOnUnload>();
    auto unloaded = manager.unloadScene(loaded.scene_id);
    check(unloaded.status == SceneStatus::CompletedWithObserverFailure && unloaded.cleanup_complete &&
          g_unload_attempts == 1 && manager.getScene(loaded.scene_id) == nullptr,
          name, "observer failure did not complete exactly once");
    auto stale = manager.resume(unloaded.operation_handle);
    check(stale.status == SceneStatus::InvalidArgument && stale.cleanup_complete && g_unload_attempts == 1,
          name, "terminal operation replayed unload event");
    EventBus::get().sink<SceneUnloadEvent>().disconnect<&throwOnUnload>();
    completed(manager.shutdown(), name, "shutdown failed");
}

void testIncompleteShutdownDiagnostic() {
    const std::string name = "destructor reports incomplete explicit shutdown";
    TempLevel level("missing-shutdown");
    if (!level.write(R"({"metadata":{"level_name":"diagnostic"},"entities":[]})")) {
        check(false, name, "could not write fixture"); return;
    }
    auto previous_logger = EE::CLog::GetEngineLogger();
    int diagnostics = 0;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&](const spdlog::details::log_msg& message) {
            const std::string payload(message.payload.begin(), message.payload.end());
            if (payload.find("SceneManager destroyed with incomplete explicit shutdown") != std::string::npos)
                ++diagnostics;
        });
    EE::CLog::GetEngineLogger() = std::make_shared<spdlog::logger>("scene-test", sink);
    EE::CLog::GetEngineLogger()->set_level(spdlog::level::trace);
    {
        world game_world;
        {
            SceneManager manager;
            completed(manager.initialize(&game_world, nullptr), name, "initialize failed");
            auto loaded = manager.loadScene(level.path.string());
            completed(loaded, name, "load failed");
            // Deliberately omit shutdown for a parsed-only scene; no entity or
            // provider retirement is needed by this diagnostic fixture.
        }
    }
    check(diagnostics == 1, name, "missing shutdown was not reported once");
    {
        world game_world;
        SceneManager manager;
        completed(manager.initialize(&game_world, nullptr), name, "second initialize failed");
        auto loaded = manager.loadScene(level.path.string());
        completed(loaded, name, "second load failed");
        completed(manager.shutdown(), name, "explicit shutdown failed");
    }
    check(diagnostics == 1, name, "completed shutdown produced a diagnostic");
    EE::CLog::GetEngineLogger() = std::move(previous_logger);
}

void testSceneOwnsOnlyInstantiatedEntities()
{
    const std::string name = "scene unload destroys only its owned entities";
    TempLevel level("ownership");
    if (!level.write(R"({"metadata":{"level_name":"ownership-test"},"entities":[{"name":"owned-freecam","type":"Freecam"},{"name":"owned-static","type":"Static"}]})"))
    {
        check(false, name, "could not write ownership fixture");
        return;
    }

    world game_world;
    HeadlessRenderAPI render_api;
    SceneManager manager;
    completed(manager.initialize(&game_world, &render_api), name, "initialize failed");

    const entt::entity unrelated = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(unrelated, "outside-scene");

    auto load = manager.loadScene(level.path.string());
    completed(load, name, "load failed");
    const SceneId id = load.scene_id;
    check(manager.getSceneState(id) == SceneLoadState::Loaded,
          name, "fixture level did not load");

    SceneResult off_owner_result;
    std::thread activation_attempt([&]
    {
        off_owner_result = manager.activateScene(id);
    });
    activation_attempt.join();
    check(off_owner_result.status == SceneStatus::WrongThread && off_owner_result.cleanup_complete, name,
          "activation from a non-owner thread was not rejected");
    check(manager.getSceneState(id) == SceneLoadState::Loaded,
          name, "rejected activation changed scene state");

    completed(manager.activateScene(id), name, "headless scene activation failed");

    Scene* scene = manager.getScene(id);
    if (!scene)
    {
        check(false, name, "activated scene record disappeared");
        completed(manager.shutdown(), name, "shutdown failed");
        return;
    }
    check(scene->owned_entities.size() == 2,
          name, "scene did not retain the exact created entity refs");
    const auto owned_handles = scene->owned_entities;
    check(owned_handles.size() == 2,
          name, "owned refs did not resolve to both instantiated entities");

    completed(manager.unloadScene(id), name, "unload failed");
    check(game_world.registry.valid(unrelated), name,
          "scene unload unexpectedly destroyed the unrelated entity");
    for (auto entity : owned_handles)
        check(game_world.classifyEntityHandle(entity) == EntityHandleClassification::RetiredGone, name,
              "scene-owned entity remained after unload");

    completed(manager.transition(level.path.string()), name, "headless scene transition failed");
    check(game_world.registry.valid(unrelated), name,
          "scene transition cleared an unrelated entity");
    completed(manager.clear(), name, "clear failed");
    completed(manager.shutdown(), name, "shutdown failed");
    check(game_world.registry.valid(unrelated), name,
          "scene clear destroyed an unrelated entity");
}
} // namespace

int main()
{
    testAsyncLoadSuccessAndFailure();
    testCancelAndClearPendingLoads();
    testShutdownDuringLoad();
    testSceneOwnsOnlyInstantiatedEntities();
    testOperationIdentityAndHeadless();
    testPublicationFailureAndRetry();
    testThrowingUnloadObserver();
    testIncompleteShutdownDiagnostic();

    if (g_failures == 0)
        std::cout << "All SceneManagerTests passed" << std::endl;
    else
        std::cerr << g_failures << " SceneManagerTests failed" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
