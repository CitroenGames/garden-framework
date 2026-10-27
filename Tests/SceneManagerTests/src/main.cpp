#include "Graphics/HeadlessRenderAPI.hpp"
#include "Scene/SceneManager.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "Components/IdentityComponent.hpp"
#include "world.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
        manager.pumpAsyncLoads();
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
    SceneManager manager;
    manager.initialize(nullptr, nullptr);
    const SceneId success_id = manager.loadSceneAsync(valid.path.string());
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
    const SceneId failed_id = manager.loadSceneAsync(missing.path.string());
    check(waitForLoad(manager, failed_id), name, "missing-file load did not complete");
    check(manager.getSceneState(failed_id) == SceneLoadState::Failed,
          name, "missing file did not produce Failed state");
    check(g_completion_count == 2 && !g_last_completion_success,
          name, "failure completion event was not published on the owner thread");
    check(g_completion_thread == owner_thread,
          name, "failure completion event ran on the worker thread");
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

    SceneManager manager;
    manager.initialize(nullptr, nullptr);
    const SceneId unloaded_id = manager.loadSceneAsync(valid.path.string());
    manager.unloadScene(unloaded_id);
    check(manager.getSceneState(unloaded_id) == SceneLoadState::Unloaded,
          name, "unloaded async scene remained registered");
    check(manager.pumpAsyncLoads() == 0,
          name, "unloaded scene produced a late completion");

    const SceneId cleared_id = manager.loadSceneAsync(valid.path.string());
    manager.clear();
    check(manager.getSceneState(cleared_id) == SceneLoadState::Unloaded,
          name, "clear retained a pending scene");
    check(manager.pumpAsyncLoads() == 0,
          name, "clear left a worker result available for late commit");
}

void testManagerDestructionDuringLoad()
{
    const std::string name = "manager destruction joins its loading worker";
    resetCompletionEvents();
    EventBus::get().sink<SceneLoadCompleteEvent>().connect<&recordCompletion>();
    TempLevel valid("manager-lifetime");
    if (!valid.write(R"({"metadata":{"level_name":"lifetime-test"},"entities":[]})"))
    {
        check(false, name, "could not write level fixture");
        return;
    }

    {
        SceneManager manager;
        manager.initialize(nullptr, nullptr);
        check(manager.loadSceneAsync(valid.path.string()) != INVALID_SCENE,
              name, "could not start async load");
    }
    check(g_completion_count == 0, name,
          "worker published a completion event without owner-thread pumping");
    resetCompletionEvents();
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
    manager.initialize(&game_world, &render_api);

    const entt::entity unrelated = game_world.registry.create();
    game_world.registry.emplace<TagComponent>(unrelated, "outside-scene");

    const SceneId id = manager.loadScene(level.path.string());
    check(manager.getSceneState(id) == SceneLoadState::Loaded,
          name, "fixture level did not load");

    bool off_owner_thread_activated = true;
    std::thread activation_attempt([&]
    {
        off_owner_thread_activated = manager.activateScene(id);
    });
    activation_attempt.join();
    check(!off_owner_thread_activated, name,
          "activation from a non-owner thread was not rejected");
    check(manager.getSceneState(id) == SceneLoadState::Loaded,
          name, "rejected activation changed scene state");

    check(manager.activateScene(id), name, "headless scene activation failed");

    Scene* scene = manager.getScene(id);
    if (!scene)
    {
        check(false, name, "activated scene record disappeared");
        return;
    }
    check(scene->owned_entities.size() == 2,
          name, "scene did not retain the exact created entity refs");
    const std::vector<EntityRef> owned_refs = scene->owned_entities;
    std::vector<entt::entity> owned_handles;
    for (EntityRef ref : owned_refs)
    {
        const entt::entity entity = resolveEntityRef(game_world.registry, ref);
        if (entity != entt::null)
            owned_handles.push_back(entity);
    }
    check(owned_handles.size() == 2,
          name, "owned refs did not resolve to both instantiated entities");

    manager.unloadScene(id);
    check(game_world.registry.valid(unrelated), name,
          "scene unload unexpectedly destroyed the unrelated entity");
    for (entt::entity entity : owned_handles)
        check(!game_world.registry.valid(entity), name,
              "scene-owned entity remained after unload");

    check(manager.transition(level.path.string()), name,
          "headless scene transition failed");
    check(game_world.registry.valid(unrelated), name,
          "scene transition cleared an unrelated entity");
    manager.clear();
    check(game_world.registry.valid(unrelated), name,
          "scene clear destroyed an unrelated entity");
}
} // namespace

int main()
{
    testAsyncLoadSuccessAndFailure();
    testCancelAndClearPendingLoads();
    testManagerDestructionDuringLoad();
    testSceneOwnsOnlyInstantiatedEntities();

    if (g_failures == 0)
        std::cout << "All SceneManagerTests passed" << std::endl;
    else
        std::cerr << g_failures << " SceneManagerTests failed" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
