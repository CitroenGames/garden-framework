#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "Utils/CrashHandler.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
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
static ReflectionRegistry reflection;
static bool game_module_resources_released = false;
static SystemScheduler* active_scheduler = nullptr;
static GameModuleHostSession game_module(GameModuleHostSession::HostOperations{
    {},
    []() {
        if (active_scheduler) {
            if (!active_scheduler->drainRetirements().cleanupComplete()) return false;
            active_scheduler->bindWorld(nullptr);
            active_scheduler = nullptr;
        }
        if (!_world.shutdown().cleanupComplete()) return false;
        game_module_resources_released = true;
        Assets::AssetManager::get().shutdown();
        return true;
    },
    {}, {}, {},
    [](const garden::ecs::ComponentOwnerId& owner) {
        Threading::JobSystem::get().barrier();
        return Threading::JobSystem::get().getPendingJobCount() == 0 &&
               EventBus::moduleSourceCanRetire(owner);
    }});

static void shutdown_server(int code)
{
    // Stop the host session while the world and scheduler-owned state remain
    // valid. The session dispatches server shutdown before unloading the DLL.
    if (!game_module.stop())
    {
        LOG_ENGINE_ERROR("Game module retirement refused: {}", game_module.lastFailure());
        std::_Exit(1);
    }
    if (!game_module_resources_released)
    {
        if (active_scheduler) {
            if (!active_scheduler->drainRetirements().cleanupComplete()) std::_Exit(1);
            active_scheduler->bindWorld(nullptr);
            active_scheduler = nullptr;
        }
        if (!_world.shutdown().cleanupComplete()) {
            LOG_ENGINE_ERROR("World shutdown incomplete");
            std::_Exit(1);
        }
        Assets::AssetManager::get().shutdown();
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
    if (!_world.resetWorld().cleanupComplete()) {
        LOG_ENGINE_FATAL("World reset incomplete");
        shutdown_server(1);
    }

    registerEngineReflection(reflection);
    level_manager.setReflectionRegistry(&reflection);

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

    // Load level
    LevelData level_data;
    const auto selected_level_artifact = ProjectManager::normalizeSelectedLevelArtifactPath(
        project_manager.getDescriptor().default_level);
    if (!project_manager.getDescriptor().default_level.empty() && !selected_level_artifact)
    {
        LOG_ENGINE_FATAL("Selected level path is invalid or escapes the project: {}",
            project_manager.getDescriptor().default_level);
        shutdown_server(1);
    }
    std::string level_path = selected_level_artifact.value_or("");
    uint32_t loaded_level_hash = 0;
    if (!level_path.empty())
    {
        level_path = Assets::AssetManager::get().resolveAssetPath(level_path);
        if (!level_manager.loadLevel(level_path, level_data))
        {
            LOG_ENGINE_FATAL("Failed to load level: {}", level_path);
            shutdown_server(1);
        }
        loaded_level_hash = Net::computeLevelHashFromFile(level_path);
        if (loaded_level_hash == 0)
        {
            LOG_ENGINE_ERROR("Loaded level '{}' could not be hashed for the network handshake", level_path);
            shutdown_server(1);
        }

        if (!level_manager.instantiateLevel(level_data, _world, render_api))
        {
            LOG_ENGINE_FATAL("Failed to instantiate level");
            shutdown_server(1);
        }

        _world.initializeGameplayFramework(level_data.metadata.level_name, "");
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
    services.level_hash = loaded_level_hash;
    services.selected_level_path = selected_level_artifact ? selected_level_artifact->c_str() : nullptr;
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

    while (running)
    {
        Uint64 frame_start_ns = SDL_GetTicksNS();
        Uint64 frame_start = SDL_GetTicks();
        float delta_time = (frame_start - delta_last) / 1000.0f;
        delta_last = frame_start;

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
