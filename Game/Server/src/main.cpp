#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "Utils/CrashHandler.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Scene/SceneManager.hpp"
#include <vector>
#include "Plugin/GameModuleHostSession.hpp"
#include "Project/ProjectManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Reflection/EngineReflection.hpp"
#include "Prefab/PrefabManager.hpp"
#include "Assets/AssetManager.hpp"
#include "Threading/JobSystem.hpp"
#include "Events/EventBus.hpp"
#include "Scheduler/SystemScheduler.hpp"
#include "Scheduler/HierarchyTransformSystem.hpp"
#include "Gameplay/GameplaySystems.hpp"
#include "Network/LevelHash.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

static Application app;
static world _world;
static ProjectManager project_manager;
static LevelManager level_manager;
static SceneManager scene_manager;
static ReflectionRegistry reflection;
static bool game_module_resources_released = false;
static SystemScheduler* active_scheduler = nullptr;
static LifecycleResult server_cleanup_result{};

// Preserve an operation returned by a host cleanup step until it is complete
// and acknowledged. A blocked_by token belongs to another operation and must
// not be resumed or acknowledged by this host cleanup path.
static bool settleServerCleanupOperation()
{
    const auto operation = server_cleanup_result.operation;
    if (!operation.valid()) return true;
    if (!server_cleanup_result.cleanupComplete()) {
        const auto resumed = _world.resumeRetirement(operation);
        if (resumed.operation != operation) return false;
        server_cleanup_result = resumed;
    }
    if (!server_cleanup_result.cleanupComplete()) return false;
    const auto acknowledged = _world.acknowledgeRetirement(operation);
    if (!acknowledged.cleanupComplete() || acknowledged.operation != operation) {
        if (acknowledged.operation == operation) server_cleanup_result = acknowledged;
        return false;
    }
    server_cleanup_result = {};
    return true;
}

static bool releaseServerResources()
{
    if (!settleServerCleanupOperation()) return false;
    if (active_scheduler) {
        server_cleanup_result = active_scheduler->drainRetirements();
        if (!server_cleanup_result.cleanupComplete() || server_cleanup_result.operation.valid()) return false;
        active_scheduler->bindWorld(nullptr);
        active_scheduler = nullptr;
    }
    server_cleanup_result = _world.shutdown();
    if (!server_cleanup_result.cleanupComplete() || server_cleanup_result.operation.valid()) return false;
    server_cleanup_result = {};
    game_module_resources_released = true;
    Assets::AssetManager::get().shutdown();
    return true;
}

static GameModuleHostSession game_module(GameModuleHostSession::HostOperations{
    {},
    []() { return releaseServerResources(); },
    {}, {}, {},
    [](const garden::ecs::ComponentOwnerId& owner) {
        Threading::JobSystem::get().barrier();
        return Threading::JobSystem::get().getPendingJobCount() == 0 &&
               EventBus::moduleSourceCanRetire(owner);
    }});

static void shutdown_server(int code)
{
    auto scene_result = scene_manager.shutdown();
    bool finishing_other_operation = scene_result.status == SceneStatus::Busy;
    for (;;) {
        if (finishing_other_operation && scene_result.cleanup_complete && scene_result.status != SceneStatus::Busy) {
            finishing_other_operation = false;
            scene_result = scene_manager.shutdown();
        }
        if (scene_result.status == SceneStatus::Completed ||
            scene_result.status == SceneStatus::CompletedWithObserverFailure ||
            scene_result.status == SceneStatus::NotInitialized) break;
        LOG_ENGINE_ERROR("Scene shutdown incomplete (status={}); retrying", static_cast<int>(scene_result.status));
        SDL_Delay(1);
        if (scene_result.operation_handle.valid() &&
            (scene_result.status == SceneStatus::Busy || !scene_result.cleanup_complete)) {
            scene_result = scene_manager.resume(scene_result.operation_handle);
        } else {
            scene_result = scene_manager.shutdown();
        }
    }
    // Stop the host session while the world and scheduler-owned state remain
    // valid. The session dispatches server shutdown before unloading the DLL.
    std::string last_module_failure;
    bool reported_module_failure = false;
    while (!game_module.stop())
    {
        if (!reported_module_failure || game_module.lastFailure() != last_module_failure) {
            last_module_failure = game_module.lastFailure();
            reported_module_failure = true;
            LOG_ENGINE_ERROR("Game module retirement refused; retrying: {}", last_module_failure);
        }
        SDL_Delay(10);
    }
    if (!game_module_resources_released)
    {
        auto last_status = LifecycleStatus::Completed;
        bool reported = false;
        while (!releaseServerResources()) {
            if (!reported || server_cleanup_result.status != last_status) {
                last_status = server_cleanup_result.status;
                reported = true;
                LOG_ENGINE_ERROR("Server resource cleanup incomplete (status={}); retrying", static_cast<int>(last_status));
            }
            SDL_Delay(10);
        }
    }
    app.shutdown();
    EE::CLog::Shutdown();
    exit(code);
}

static std::string parseProjectPath(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--project") == 0)
            return argv[i + 1];
    }
    return "";
}

static uint16_t parsePort(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--port") == 0)
            return static_cast<uint16_t>(atoi(argv[i + 1]));
    }
    return 0;
}

static std::string findGardenFile(const fs::path& dir)
{
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return "";
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".garden")
            return entry.path().string();
    }
    return "";
}

int main(int argc, char* argv[])
{
    Paingine2D::CrashHandler* crashHandler = Paingine2D::CrashHandler::GetInstance();
    crashHandler->Initialize("Server");
    EE::CLog::Init();

    LOG_ENGINE_INFO("Starting Dedicated Server...");

    // Find the .garden project file
    std::string garden_path = parseProjectPath(argc, argv);
    if (garden_path.empty())
    {
        fs::path exe_dir = EnginePaths::getExecutableDir();
        garden_path = findGardenFile(exe_dir / "..");
    }

    if (garden_path.empty())
    {
        LOG_ENGINE_FATAL("No .garden project file found. Pass --project <path> or place a .garden file in the parent directory.");
        return 1;
    }

    if (!project_manager.loadProject(garden_path))
    {
        LOG_ENGINE_FATAL("Failed to load project: {}", garden_path);
        return 1;
    }

    fs::current_path(project_manager.getProjectRoot());
    LOG_ENGINE_INFO("Project '{}' loaded", project_manager.getDescriptor().name);
    level_manager.setGameplayDefaults(project_manager.getDescriptor().default_game_mode,
                                      project_manager.getDescriptor().default_game_state);

    // Initialize headless application (no window, no GPU)
    app = Application(1, 1, 60, 75.0f, RenderAPIType::Headless);
    if (!app.initialize("Server", false))
    {
        LOG_ENGINE_FATAL("Failed to initialize server application");
        return 1;
    }

    IRenderAPI* render_api = app.getRenderAPI();

    // Initialize AssetManager for path resolution
    Assets::AssetManager::get().initialize(render_api);
    {
        const auto& asset_dir = project_manager.getDescriptor().asset_directories[0];
        Assets::AssetManager::get().setAssetRoot(project_manager.resolveProjectPath(asset_dir));
        Assets::AssetManager::get().setAssetPrefix(asset_dir);
    }

    // Initialize world and physics
    server_cleanup_result = _world.resetWorld();
    if (!server_cleanup_result.cleanupComplete() || server_cleanup_result.operation.valid()) {
        LOG_ENGINE_FATAL("World reset incomplete");
        shutdown_server(1);
    }
    server_cleanup_result = {};

    registerEngineReflection(reflection);
    level_manager.setReflectionRegistry(&reflection);
    if (scene_manager.initialize(&_world, render_api).status != SceneStatus::Completed ||
        scene_manager.setReflectionRegistry(&reflection).status != SceneStatus::Completed ||
        scene_manager.setGameplayDefaults(project_manager.getDescriptor().default_game_mode,
                                          project_manager.getDescriptor().default_game_state).status != SceneStatus::Completed) {
        LOG_ENGINE_FATAL("SceneManager initialization failed");
        shutdown_server(1);
    }

    // Load game DLL before the level so reflected game components can be imported.
    std::string dll_path = project_manager.getAbsoluteModulePath();
    if (!game_module.loadAndRegister(dll_path, reflection))
    {
        LOG_ENGINE_FATAL("Failed to load game module: {}", dll_path);
        shutdown_server(1);
    }

    if (!game_module.loader().hasServerSupport())
    {
        LOG_ENGINE_FATAL("Game module '{}' does not export server hooks", game_module.loader().getGameName());
        shutdown_server(1);
    }

    // Validate every requested artifact before the first transition.
    struct SelectedLevel { std::string artifact, resolved; uint32_t hash = 0; };
    auto selectLevel = [](const std::string& path, SelectedLevel& out) {
        auto normalized = ProjectManager::normalizeSelectedLevelArtifactPath(path);
        if (!normalized || normalized->empty()) return false;
        out.artifact = *normalized;
        out.resolved = Assets::AssetManager::get().resolveAssetPath(out.artifact);
        out.hash = Net::computeLevelHashFromFile(out.resolved);
        return out.hash != 0;
    };
    SelectedLevel selected;
    if (!selectLevel(project_manager.getDescriptor().default_level, selected)) {
        LOG_ENGINE_FATAL("Invalid or unhashable default level: {}", project_manager.getDescriptor().default_level);
        shutdown_server(1);
    }
    std::vector<SelectedLevel> transitions;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--transition-level") != 0) continue;
        if (++i >= argc || argv[i][0] == '-') {
            LOG_ENGINE_FATAL("--transition-level requires a level path"); shutdown_server(1);
        }
        SelectedLevel candidate;
        if (!selectLevel(argv[i], candidate)) {
            LOG_ENGINE_FATAL("Invalid or unhashable transition level: {}", argv[i]); shutdown_server(1);
        }
        transitions.push_back(std::move(candidate));
    }
    auto initial = scene_manager.loadScene(selected.resolved);
    if (initial.status != SceneStatus::Completed) {
        LOG_ENGINE_FATAL("Failed to load level: {}", selected.resolved); shutdown_server(1);
    }
    auto activated = scene_manager.activateScene(initial.scene_id);
    while (!activated.cleanup_complete && activated.operation_handle.valid())
        activated = scene_manager.resume(activated.operation_handle);
    if (activated.status != SceneStatus::Completed && activated.status != SceneStatus::CompletedWithObserverFailure) {
        LOG_ENGINE_FATAL("Failed to activate level (status={})", static_cast<int>(activated.status)); shutdown_server(1);
    }
    if (!_world.initializeGameplayFramework(scene_manager.getActiveScene()->level_data.metadata.level_name, "")) {
        LOG_ENGINE_FATAL("Gameplay framework initialization failed"); shutdown_server(1);
    }

    // Initialize server via DLL
    uint16_t listen_port = parsePort(argc, argv);

    EngineServices services{};
    services.game_world = &_world;
    services.render_api = render_api;
    services.input_manager = nullptr;  // no input on server
    services.reflection = &reflection;
    services.application = &app;
    services.level_manager = &level_manager;
    services.api_version = GARDEN_MODULE_API_VERSION;
    services.level_hash = selected.hash;
    services.selected_level_path = selected.artifact.c_str();
    services.listen_port = listen_port;

    PrefabManager::get().initialize(&reflection, render_api);

    if (!game_module.initialiseServer(&services))
    {
        LOG_ENGINE_FATAL("Server module initialization failed");
        shutdown_server(1);
    }

    if (!game_module.notifyServerLevelLoaded())
    {
        LOG_ENGINE_FATAL("Server module level-loaded callback could not be dispatched");
        shutdown_server(1);
    }
    _world.startGameplayFramework();
    if (!game_module.beginPlaying(false))
    {
        LOG_ENGINE_FATAL("Server module did not reach the playing lifecycle phase");
        shutdown_server(1);
    }

    // D5: dedicated server is driven by a SystemScheduler (server host).
    // World physics is advanced once per consumed fixed tick in Physics,
    // before the game module's server gameplay callback.
    SystemScheduler server_scheduler(true, _world.getFixedDelta());
    server_scheduler.bindWorld(&_world);
    active_scheduler = &server_scheduler;
    registerGameplaySystems(server_scheduler);
    {
        SystemDescriptor d;
        d.name = "game.server_pre_physics";
        d.phase = Phase::PrePhysics;
        d.update = [](entt::registry&, const Tick::SimulationTickContext& ctx) {
            game_module.updateServerPrePhysics(ctx.fixed_delta);
        };
        server_scheduler.addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.physics";
        d.phase = Phase::Physics;
        d.update = [](entt::registry&, const Tick::SimulationTickContext&) {
            _world.stepPhysicsSingleStep();
        };
        server_scheduler.addSystem(std::move(d));
    }
    // PhysicsSystem polls triggers after each Jolt step, so the server does
    // not register a second trigger query system here.
    {
        SystemDescriptor d;
        d.name = "game.gameplay_framework";
        d.phase = Phase::Gameplay;
        d.update = [](entt::registry&, const Tick::SimulationTickContext& ctx) {
            _world.tickGameplayFramework(ctx.fixed_delta);
        };
        server_scheduler.addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "game.server_update";
        d.phase = Phase::Gameplay;
        d.run_after = {"game.gameplay_framework"};
        d.update = [](entt::registry&, const Tick::SimulationTickContext& ctx) {
            game_module.updateServer(ctx.fixed_delta);
        };
        server_scheduler.addSystem(std::move(d));
    }
    registerHierarchyTransformSystem(server_scheduler);

    LOG_ENGINE_INFO("Server started successfully");

    // Server loop
    Uint64 delta_last = SDL_GetTicks();
    bool running = true;
    size_t transition_index = 0;
    SceneOperationHandle pending_transition;

    while (running)
    {
        Uint64 frame_start_ns = SDL_GetTicksNS();
        Uint64 frame_start = SDL_GetTicks();
        float delta_time = (frame_start - delta_last) / 1000.0f;
        delta_last = frame_start;

        if (transition_index < transitions.size()) {
            auto& next = transitions[transition_index];
            auto result = pending_transition.valid() ? scene_manager.resume(pending_transition)
                                                    : scene_manager.transition(next.resolved);
            if (!result.cleanup_complete && result.operation_handle.valid()) {
                pending_transition = result.operation_handle;
                LOG_ENGINE_ERROR("Level transition incomplete (status={}); retrying", static_cast<int>(result.status));
                SDL_Event pending_event;
                while (SDL_PollEvent(&pending_event))
                    if (pending_event.type == SDL_EVENT_QUIT) running = false;
                SDL_Delay(1);
                continue; // The scheduler must not drain a manager-owned token.
            }
            pending_transition = {};
            if (result.status != SceneStatus::Completed && result.status != SceneStatus::CompletedWithObserverFailure) {
                LOG_ENGINE_ERROR("Level transition failed (status={})", static_cast<int>(result.status));
                running = false;
                continue;
            }
            selected = next;
            services.selected_level_path = selected.artifact.c_str();
            services.level_hash = selected.hash;
            if (!_world.initializeGameplayFramework(scene_manager.getActiveScene()->level_data.metadata.level_name, "")) {
                LOG_ENGINE_ERROR("Gameplay framework initialization failed after transition"); running = false; continue;
            }
            if (!game_module.notifyServerLevelTransition()) {
                LOG_ENGINE_ERROR("Server level transition notification failed"); running = false; continue;
            }
            _world.startGameplayFramework();
            LOG_ENGINE_INFO("Server transitioned to '{}' (hash={})", selected.artifact, selected.hash);
            ++transition_index;
        }
        server_scheduler.runFrame(_world.registry, delta_time);
        if (!server_scheduler.lastRetirementResult().cleanupComplete()) {
            LOG_ENGINE_ERROR("Server retirement drain failed (status={})", static_cast<int>(server_scheduler.lastRetirementResult().status));
            running = false;
        }

        Uint64 frame_end_ns = SDL_GetTicksNS();
        app.lockFramerate(frame_start_ns, frame_end_ns);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }
    }

    shutdown_server(0);
    return 0;
}
