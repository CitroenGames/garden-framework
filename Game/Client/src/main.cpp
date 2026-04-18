#include "Utils/CrashHandler.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <filesystem>
#include <optional>

#include "Application.hpp"
#include "world.hpp"
#include "LevelManager.hpp"
#include "Graphics/renderer.hpp"
#include "Plugin/GameModuleLoader.hpp"
#include "Project/ProjectManager.hpp"
#include "InputHandler.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "UI/RmlUiManager.h"
#include "Console/ConVar.hpp"
#include "Threading/JobSystem.hpp"
#include "Assets/AssetManager.hpp"
#include "Assets/GltfAssetLoader.hpp"
#include "Audio/AudioSystem.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "Prefab/PrefabManager.hpp"

namespace fs = std::filesystem;

static Application app;
static renderer _renderer;
static world _world;
static InputHandler input_handler;
static GameModuleLoader game_module;
static ProjectManager project_manager;
static LevelManager level_manager;
static ReflectionRegistry reflection;

static void quit_game(int code)
{
    ConVarRegistry::get().saveArchiveCvars("config.cfg");

    if (game_module.isLoaded())
    {
        game_module.shutdown();
        game_module.unload();
    }
    AudioSystem::get().shutdown();
    Assets::AssetManager::get().shutdown();
    Threading::JobSystem::get().shutdown();
    _world.registry.clear();
    RmlUiManager::get().shutdown();
    ImGuiManager::get().shutdown();
    app.shutdown();
    EE::CLog::Shutdown();
    _exit(code);
}

static std::optional<RenderAPIType> parseRenderAPICLI(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-vulkan") == 0 || strcmp(argv[i], "--vulkan") == 0)
            return RenderAPIType::Vulkan;
#ifdef __APPLE__
        if (strcmp(argv[i], "-metal") == 0 || strcmp(argv[i], "--metal") == 0)
            return RenderAPIType::Metal;
#endif
#ifdef _WIN32
        if (strcmp(argv[i], "-d3d12") == 0 || strcmp(argv[i], "--d3d12") == 0 ||
            strcmp(argv[i], "-dx12") == 0 || strcmp(argv[i], "--dx12") == 0)
            return RenderAPIType::D3D12;
#endif
    }
    return std::nullopt;
}

static RenderAPIType resolveRenderAPI(int argc, char* argv[])
{
    // 1. CLI flags take highest priority
    auto cli_override = parseRenderAPICLI(argc, argv);
    if (cli_override.has_value())
    {
        RenderAPIType api = cli_override.value();
        if (IsRenderAPIPlatformAvailable(api))
        {
            if (auto* cvar = CVAR_PTR(r_renderapi))
                cvar->setString(RenderAPITypeToString(api));
            LOG_ENGINE_INFO("Render API override from CLI: {}", RenderAPITypeToString(api));
            return api;
        }
        LOG_ENGINE_WARN("CLI-specified render API '{}' not available on this platform, falling back",
                        RenderAPITypeToString(api));
    }

    // 2. Read from the r_renderapi convar (loaded from config.cfg)
    if (auto* cvar = CVAR_PTR(r_renderapi))
    {
        const std::string& saved = cvar->getString();
        if (!saved.empty())
        {
            RenderAPIType api = ParseRenderAPIType(saved);
            if (IsRenderAPIPlatformAvailable(api))
            {
                LOG_ENGINE_INFO("Render API from config: {}", RenderAPITypeToString(api));
                return api;
            }
            LOG_ENGINE_WARN("Saved render API '{}' not available on this platform, using default", saved);
        }
    }

    // 3. Platform default
    LOG_ENGINE_INFO("Using default render API: {}", RenderAPITypeToString(DefaultRenderAPI));
    return DefaultRenderAPI;
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

static std::string parseConnectAddress(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--connect") == 0)
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

// Find a .garden file in the given directory
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

#if _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char* argv[])
#endif
{
    Paingine2D::CrashHandler* crashHandler = Paingine2D::CrashHandler::GetInstance();
    crashHandler->Initialize("Game");
    EE::CLog::Init();

    InitializeDefaultCVars();
    ConVarRegistry::get().loadConfig("config.cfg");

    // Warn that r_renderapi changes require a restart
    if (auto* cvar = CVAR_PTR(r_renderapi))
    {
        cvar->addChangeCallback([](ConVarBase*, const ConVarValue&, const ConVarValue&) {
            LOG_ENGINE_WARN("r_renderapi changed. Restart the game for the change to take effect.");
        });
    }

#if _WIN32
    int argc = __argc;
    char** argv = __argv;
#endif
    RenderAPIType api_type = resolveRenderAPI(argc, argv);

    // Find the .garden project file
    std::string garden_path = parseProjectPath(argc, argv);
    if (garden_path.empty())
    {
        // Look in ../ relative to the executable
        fs::path exe_dir = EnginePaths::getExecutableDir();
        garden_path = findGardenFile(exe_dir / "..");
    }

    if (garden_path.empty())
    {
        LOG_ENGINE_FATAL("No .garden project file found. Pass --project <path> or place a .garden file in the parent directory.");
        _exit(1);
    }

    // Load the project
    if (!project_manager.loadProject(garden_path))
    {
        LOG_ENGINE_FATAL("Failed to load project: {}", garden_path);
        _exit(1);
    }

    // Change working directory to project root
    fs::current_path(project_manager.getProjectRoot());
    LOG_ENGINE_INFO("Project '{}' loaded from '{}'",
                    project_manager.getDescriptor().name,
                    project_manager.getProjectRoot());

    // Initialize application
    app = Application(1920, 1080, 60, 75.0f, api_type);
#ifdef _DEBUG
    bool start_fullscreen = false;
#else
    bool start_fullscreen = true;
#endif
    if (!app.initialize(project_manager.getDescriptor().name.c_str(), start_fullscreen))
        quit_game(1);

    IRenderAPI* render_api = app.getRenderAPI();
    if (!render_api)
    {
        LOG_ENGINE_FATAL("Failed to get render API");
        quit_game(1);
    }

    LOG_ENGINE_TRACE("Game initialized with {} render API", render_api->getAPIName());

    if (!ImGuiManager::get().initialize(app.getWindow(), render_api, api_type))
    {
        LOG_ENGINE_FATAL("Failed to initialize ImGui");
        quit_game(1);
    }

    if (!RmlUiManager::get().initialize(app.getWindow(), render_api, api_type))
        LOG_ENGINE_WARN("Failed to initialize RmlUi - continuing without UI");

    if (!Threading::JobSystem::get().initialize())
    {
        LOG_ENGINE_FATAL("Failed to initialize Job System");
        quit_game(1);
    }

    if (!Assets::AssetManager::get().initialize(render_api))
    {
        LOG_ENGINE_FATAL("Failed to initialize Asset Manager");
        quit_game(1);
    }
    {
        const auto& asset_dir = project_manager.getDescriptor().asset_directories[0];
        Assets::AssetManager::get().setAssetRoot(project_manager.resolveProjectPath(asset_dir));
        Assets::AssetManager::get().setAssetPrefix(asset_dir);
    }
    Assets::AssetManager::get().registerLoader(std::make_unique<Assets::GltfAssetLoader>());

    if (!AudioSystem::get().initialize())
        LOG_ENGINE_WARN("Failed to initialize Audio System - continuing without audio");

    // Set up input
    input_handler.set_window(app.getWindow());
    input_handler.set_quit_callback([]() { quit_game(0); });
    input_handler.set_resize_callback([](int w, int h) { app.onWindowResized(w, h); });
    auto input_manager = input_handler.get_input_manager();

    // Initialize world and physics
    _world = world();
    _world.initializePhysics();

    // Load default level
    LevelData level_data;
    std::string level_path = project_manager.getDescriptor().default_level;
    if (!level_path.empty())
    {
        level_path = Assets::AssetManager::get().resolveAssetPath(level_path);
        if (!level_manager.loadLevel(level_path, level_data))
        {
            LOG_ENGINE_FATAL("Failed to load level: {}", level_path);
            quit_game(1);
        }

        entt::entity player_entity = entt::null;
        entt::entity freecam_entity = entt::null;
        entt::entity player_rep_entity = entt::null;

        if (!level_manager.instantiateLevelParallel(level_data, _world, render_api,
                &player_entity, &freecam_entity, &player_rep_entity))
        {
            LOG_ENGINE_FATAL("Failed to instantiate level");
            quit_game(1);
        }
    }

    // Initialize renderer
    _renderer = renderer(render_api);

    // Apply lighting from level
    _renderer.set_level_lighting(
        level_data.metadata.ambient_light,
        level_data.metadata.diffuse_light,
        level_data.metadata.light_direction);

    // Load game DLL
    std::string dll_path = project_manager.getAbsoluteModulePath();
    if (!game_module.load(dll_path))
    {
        LOG_ENGINE_FATAL("Failed to load game module: {}", dll_path);
        quit_game(1);
    }

    // Parse network CLI args
    std::string connect_addr = parseConnectAddress(argc, argv);
    uint16_t connect_port = parsePort(argc, argv);

    // Initialize game module
    EngineServices services{};
    services.game_world = &_world;
    services.render_api = render_api;
    services.input_manager = input_manager.get();
    services.reflection = &reflection;
    services.application = &app;
    services.level_manager = &level_manager;
    services.api_version = GARDEN_MODULE_API_VERSION;
    services.connect_address = connect_addr.empty() ? nullptr : connect_addr.c_str();
    services.connect_port = connect_port;

    game_module.registerComponents(&reflection);

    PrefabManager::get().initialize(&reflection, render_api);

    if (!game_module.init(&services))
    {
        LOG_ENGINE_FATAL("Game module initialization failed");
        quit_game(1);
    }

    game_module.onLevelLoaded();

    // Main game loop
    Uint64 delta_last = SDL_GetTicks();

    while (true)
    {
        render_api->executeWithAutoreleasePool([&]() {
            Uint64 frame_start = SDL_GetTicks();

            ImGuiManager::get().newFrame();
            RmlUiManager::get().beginFrame();

            input_handler.process_events();

            if (input_handler.is_window_minimized())
            {
                SDL_Delay(10);
                return;
            }

            Threading::JobSystem::get().processMainThreadJobs();

            if (input_handler.should_quit_application())
                quit_game(0);

            float delta_time = (frame_start - delta_last) / 1000.0f;
            delta_last = frame_start;

            // Let the game DLL handle all game logic
            game_module.update(delta_time);

            // Render using the world camera (updated by the game DLL)
            _renderer.render_scene(_world.registry, _world.world_camera);

            ImGuiManager::get().updatePlatformWindows();

            app.swapBuffers();

            Uint64 frame_end = SDL_GetTicks();
            int fps_max_val = CVAR_INT(fps_max);
            if (fps_max_val > 0)
                app.setTargetFPS(fps_max_val);
            else
                app.setTargetFPS(10000);
            app.lockFramerate(frame_start, frame_end);
        });
    }

    crashHandler->Shutdown();
    _exit(0);
}
