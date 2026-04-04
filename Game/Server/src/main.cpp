#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <filesystem>

#define SDL_MAIN_HANDLED
#include "SDL.h"

#include "Utils/CrashHandler.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Plugin/GameModuleLoader.hpp"
#include "Project/ProjectManager.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

static Application app;
static world _world;
static GameModuleLoader game_module;
static ProjectManager project_manager;
static LevelManager level_manager;
static ReflectionRegistry reflection;

static void shutdown_server(int code)
{
    if (game_module.isLoaded())
    {
        game_module.serverShutdown();
        game_module.unload();
    }
    _world.registry.clear();
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

    // Initialize headless application (no window, no GPU)
    app = Application(1, 1, 60, 75.0f, RenderAPIType::Headless);
    if (!app.initialize("Server", false))
    {
        LOG_ENGINE_FATAL("Failed to initialize server application");
        return 1;
    }

    IRenderAPI* render_api = app.getRenderAPI();

    // Initialize world and physics
    _world = world();
    _world.initializePhysics();

    // Load level
    LevelData level_data;
    std::string level_path = project_manager.getDescriptor().default_level;
    if (!level_path.empty())
    {
        if (!level_manager.loadLevel(level_path, level_data))
        {
            LOG_ENGINE_FATAL("Failed to load level: {}", level_path);
            shutdown_server(1);
        }

        if (!level_manager.instantiateLevel(level_data, _world, render_api))
        {
            LOG_ENGINE_FATAL("Failed to instantiate level");
            shutdown_server(1);
        }
    }

    // Load game DLL
    std::string dll_path = project_manager.getAbsoluteModulePath();
    if (!game_module.load(dll_path))
    {
        LOG_ENGINE_FATAL("Failed to load game module: {}", dll_path);
        shutdown_server(1);
    }

    if (!game_module.hasServerSupport())
    {
        LOG_ENGINE_FATAL("Game module '{}' does not export server hooks", game_module.getGameName());
        shutdown_server(1);
    }

    // Initialize server via DLL
    EngineServices services{};
    services.game_world = &_world;
    services.render_api = render_api;
    services.input_manager = nullptr;  // no input on server
    services.reflection = &reflection;
    services.application = &app;
    services.level_manager = &level_manager;
    services.api_version = GARDEN_MODULE_API_VERSION;

    game_module.registerComponents(&reflection);

    if (!game_module.serverInit(&services))
    {
        LOG_ENGINE_FATAL("Server module initialization failed");
        shutdown_server(1);
    }

    game_module.serverOnLevelLoaded();

    LOG_ENGINE_INFO("Server started successfully");

    // Server loop
    Uint32 delta_last = SDL_GetTicks();
    bool running = true;

    while (running)
    {
        Uint32 frame_start = SDL_GetTicks();
        float delta_time = (frame_start - delta_last) / 1000.0f;
        delta_last = frame_start;

        game_module.serverUpdate(delta_time);

        Uint32 frame_end = SDL_GetTicks();
        app.lockFramerate(frame_start, frame_end);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
        }
    }

    shutdown_server(0);
    return 0;
}
