#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "Utils/CrashHandler.hpp"
#include <windows.h>
#endif

#include "math.h"
#include "SDL.h"

#include "Pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <map>

#define ENET_IMPLEMENTATION
#include "enet.h"

#include "Application.hpp"

// Components
#include "Components/Components.hpp"
#include "Components/camera.hpp" // Keep camera for now as it's used in world/renderer

#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "world.hpp"
#include "Graphics/renderer.hpp"
#include "LevelManager.hpp"
#include "ClientNetworkManager.hpp"
#include "SharedComponents.hpp"

#include "Utils/Log.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "Console/ConVar.hpp"

// Threading and Asset loading
#include "Threading/JobSystem.hpp"
#include "Assets/AssetManager.hpp"
#include "Assets/GltfAssetLoader.hpp"
#include "Assets/ModelAssetData.hpp"

static Application app;
static renderer _renderer;
static world _world;
static InputHandler input_handler;
static std::unique_ptr<PlayerController> player_controller;
static Game::ClientNetworkManager _network;

// Async loading test
static Assets::AssetHandle g_async_load_handle;
static bool g_async_load_pending = false;

static void quit_game(int code)
{
    ConVarRegistry::get().saveArchiveCvars("config.cfg");
    _network.disconnect("Game closing");
    _network.shutdown();
    Assets::AssetManager::get().shutdown();
    Threading::JobSystem::get().shutdown();
    ImGuiManager::get().shutdown();
    app.shutdown();
    EE::CLog::Shutdown();
    exit(code);
}

// System to update player representations
void update_player_representations(entt::registry& registry, bool is_freecam)
{
    auto view = registry.view<PlayerRepresentationComponent, TransformComponent, MeshComponent>();

    for(entt::entity entity : view) {
        auto& pr = view.get<PlayerRepresentationComponent>(entity);
        auto& trans = view.get<TransformComponent>(entity);
        auto& mesh_comp = view.get<MeshComponent>(entity);
        
        if (registry.valid(pr.tracked_player) && registry.all_of<TransformComponent>(pr.tracked_player)) {
            const auto& target_trans = registry.get<TransformComponent>(pr.tracked_player);
            
            // Sync position with offset
            trans.position = target_trans.position + pr.position_offset;
            
            // Sync rotation (Y only typically for character representation)
            trans.rotation.y = target_trans.rotation.y;
            // Or if we want full rotation sync:
            // trans.rotation = target_trans.rotation;
        }

        // Visibility
        if (pr.visible_only_freecam && mesh_comp.m_mesh) {
            mesh_comp.m_mesh->visible = is_freecam;
        }
    }
}

// Parse command line for render API selection
static RenderAPIType parseRenderAPI(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-vulkan") == 0 || strcmp(argv[i], "--vulkan") == 0)
        {
            return RenderAPIType::Vulkan;
        }
        if (strcmp(argv[i], "-opengl") == 0 || strcmp(argv[i], "--opengl") == 0)
        {
            return RenderAPIType::OpenGL;
        }
#ifdef _WIN32
        if (strcmp(argv[i], "-d3d11") == 0 || strcmp(argv[i], "--d3d11") == 0 ||
            strcmp(argv[i], "-dx11") == 0 || strcmp(argv[i], "--dx11") == 0)
        {
            return RenderAPIType::D3D11;
        }
#endif
    }
    return RenderAPIType::OpenGL; // Default
}

#if _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char* argv[])
#endif
{
	Pakker pak;
        pak.FileExists("example.pak", "test.txt");
    Paingine2D::CrashHandler* crashHandler = Paingine2D::CrashHandler::GetInstance();
    crashHandler->Initialize("Game");
	EE::CLog::Init();

    // Initialize console variables and load config
    InitializeDefaultCVars();
    ConVarRegistry::get().loadConfig("config.cfg");

    // Parse command line arguments
#if _WIN32
    int argc = __argc;
    char** argv = __argv;
#endif
    RenderAPIType api_type = parseRenderAPI(argc, argv);

    // Initialize application with selected render API
    app = Application(1920, 1080, 60, 75.0f, api_type);
    if (!app.initialize("Game Window", true))
    {
        quit_game(1);
    }

    // Get the render API from the application
    IRenderAPI* render_api = app.getRenderAPI();
    if (!render_api)
    {
		LOG_ENGINE_FATAL("Failed to get render API from application");
        quit_game(1);
    }

    LOG_ENGINE_TRACE("Game initialized with {0} render API", render_api->getAPIName());

    // Initialize ImGui
    if (!ImGuiManager::get().initialize(app.getWindow(), render_api, api_type))
    {
        LOG_ENGINE_FATAL("Failed to initialize ImGui");
        quit_game(1);
    }

    // Initialize Job System
    if (!Threading::JobSystem::get().initialize()) {
        LOG_ENGINE_FATAL("Failed to initialize Job System");
        quit_game(1);
    }

    // Initialize Asset Manager
    if (!Assets::AssetManager::get().initialize(render_api)) {
        LOG_ENGINE_FATAL("Failed to initialize Asset Manager");
        quit_game(1);
    }

    // Register GLTF loader
    Assets::AssetManager::get().registerLoader(std::make_unique<Assets::GltfAssetLoader>());

    // Set up input system
    input_handler.set_quit_callback([]() {
        quit_game(0);
    });

    auto input_manager = input_handler.get_input_manager();

    /* Frame locking */
    Uint32 frame_start_ticks;
    Uint32 frame_end_ticks;

    /* Create world */
    _world = world();

    /* Initialize networking */
    if (!_network.initialize()) {
        LOG_ENGINE_FATAL("Failed to initialize Client Network");
        quit_game(1);
    }

    if (!_network.connectToServer("127.0.0.1", 7777, "Player")) {
        LOG_ENGINE_FATAL("Failed to connect to server");
        quit_game(1);
    }

    _network.setWorld(&_world);

    /* Level loading */
    LevelManager level_manager;
    LevelData level_data;
    std::string level_path = "levels/main.level.json";

    printf("Loading level from: %s\n", level_path.c_str());
    if (!level_manager.loadLevel(level_path, level_data))
    {
        LOG_ENGINE_FATAL("Failed to load level: {}", level_path.c_str());
        quit_game(1);
    }

    entt::entity player_entity = entt::null;
    entt::entity freecam_entity = entt::null;
    entt::entity player_rep_entity = entt::null;

    // Instantiate level entities
    if (!level_manager.instantiateLevel(
            level_data, _world, render_api,
            &player_entity, &freecam_entity, &player_rep_entity))
    {
        LOG_ENGINE_FATAL("Failed to instantiate level");
        quit_game(1);
    }

    // Verify critical entities exist
    if (!_world.registry.valid(player_entity))
    {
        LOG_ENGINE_FATAL("Level does not contain a player entity");
        quit_game(1);
    }
    
    // Set up cameras based on entities (if they have transforms)
    if (_world.registry.valid(player_entity)) {
        auto& t = _world.registry.get<TransformComponent>(player_entity);
        _world.world_camera.position = t.position;
        _world.world_camera.rotation = t.rotation;
    }

    if (_world.registry.valid(freecam_entity)) {
        // Just ensure it exists, position is synced on toggle if needed
    }

    // Set up player controller with new input system
    player_controller = std::make_unique<PlayerController>(input_manager, &_world);
    player_controller->setPossessedPlayer(player_entity);
    player_controller->setPossessedFreecam(freecam_entity);

    /* Renderer - Using the abstracted render API */
    _renderer = renderer::renderer(render_api);
    
    // Apply lighting settings from level metadata
    _renderer.set_level_lighting(
        level_data.metadata.ambient_light,
        level_data.metadata.diffuse_light,
        level_data.metadata.light_direction
    );

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

    atexit(SDL_Quit);
    while (1)
    {
        frame_start_ticks = SDL_GetTicks();

        // Start new ImGui frame
        ImGuiManager::get().newFrame();

        // Process input events through the new input system
        input_handler.process_events();

        // Process async loading jobs (GPU uploads must happen on main thread)
        Threading::JobSystem::get().processMainThreadJobs();
        
        // Handle mouse motion for camera control (skip in UI mode)
        if (input_manager && !input_handler.is_ui_mode())
        {
            float mouse_x = input_manager->get_mouse_delta_x();
            float mouse_y = input_manager->get_mouse_delta_y();

            if (mouse_x != 0.0f || mouse_y != 0.0f)
            {
                player_controller->handleMouseMotion(mouse_y, mouse_x);
            }
        }

        // Check if quit was requested
        if (input_handler.should_quit_application())
        {
            quit_game(0);
        }

        // F3: Test async model loading - spawn Character.gltf at player position
        if (input_manager && input_manager->is_key_pressed(SDL_SCANCODE_F3))
        {
            if (!g_async_load_pending)
            {
                // Get player position for spawning
                glm::vec3 spawn_pos(0.0f);
                if (_world.registry.valid(player_entity)) {
                    auto& player_transform = _world.registry.get<TransformComponent>(player_entity);
                    spawn_pos = player_transform.position;
                }

                LOG_ENGINE_INFO("F3 pressed - Loading Character.gltf at position ({}, {}, {})",
                    spawn_pos.x, spawn_pos.y, spawn_pos.z);

                g_async_load_handle = Assets::AssetManager::get().loadAsync(
                    "models/Character.gltf",
                    Assets::LoadPriority::High,
                    [spawn_pos](Assets::AssetId id, bool success, const Assets::AssetData& data) {
                        if (success) {
                            auto model = std::get<std::shared_ptr<Assets::ModelAssetData>>(data);
                            if (model && model->mesh_data && model->mesh_data->gpu_mesh) {
                                LOG_ENGINE_INFO("Async load completed! Spawning entity...");

                                // Create mesh that shares the GPU data (don't transfer ownership)
                                auto new_mesh = std::make_shared<mesh>(nullptr, 0);
                                new_mesh->gpu_mesh = model->mesh_data->gpu_mesh;
                                new_mesh->vertices_len = model->mesh_data->getVertexCount();
                                new_mesh->is_valid = true;
                                new_mesh->owns_vertices = false;
                                // Note: We share gpu_mesh, don't set it to nullptr on model

                                // Set up material ranges from submeshes for multi-texture support
                                if (model->mesh_data && !model->mesh_data->submeshes.empty()) {
                                    std::vector<MaterialRange> ranges;

                                    for (const auto& submesh : model->mesh_data->submeshes) {
                                        TextureHandle tex = INVALID_TEXTURE;
                                        if (submesh.material_index >= 0) {
                                            tex = model->getMaterialPrimaryTexture(submesh.material_index);
                                        }
                                        ranges.emplace_back(submesh.start_vertex, submesh.vertex_count, tex, submesh.material_name);
                                    }

                                    if (!ranges.empty()) {
                                        new_mesh->setMaterialRanges(ranges);
                                        LOG_ENGINE_INFO("Set up {} material ranges for multi-texture mesh", ranges.size());
                                    }
                                }
                                // Fallback to single texture if no submeshes
                                else {
                                    TextureHandle tex = model->getMaterialPrimaryTexture(0);
                                    if (tex != INVALID_TEXTURE) {
                                        new_mesh->set_texture(tex);
                                    }
                                }

                                // Create entity in world
                                entt::entity new_entity = _world.registry.create();
                                _world.registry.emplace<TransformComponent>(new_entity,
                                    spawn_pos.x, spawn_pos.y, spawn_pos.z);
                                _world.registry.emplace<MeshComponent>(new_entity, new_mesh);
                                _world.registry.emplace<TagComponent>(new_entity, TagComponent{"AsyncCharacter"});

                                LOG_ENGINE_INFO("Spawned Character at ({}, {}, {})",
                                    spawn_pos.x, spawn_pos.y, spawn_pos.z);
                            } else {
                                LOG_ENGINE_ERROR("Model loaded but mesh data is missing!");
                            }
                        } else {
                            LOG_ENGINE_ERROR("Async load failed!");
                        }
                        g_async_load_pending = false;
                    }
                );
                g_async_load_pending = true;
            }
            else
            {
                LOG_ENGINE_WARN("Async load already in progress...");
            }
        }

        // delta time
        delta_time = (frame_start_ticks - delta_last) / 1000.0f;
        delta_last = frame_start_ticks;

        // Network update - receive world state and process messages
        _network.update(delta_time);

        // Send player input to server
        if (_network.isConnected() && input_manager)
        {
            Game::InputState input_state;
            input_state.buttons = 0;

            // Map input to button flags
            if (input_manager->is_key_held(SDL_SCANCODE_W)) input_state.buttons |= InputFlags::MOVE_FORWARD;
            if (input_manager->is_key_held(SDL_SCANCODE_S)) input_state.buttons |= InputFlags::MOVE_BACK;
            if (input_manager->is_key_held(SDL_SCANCODE_A)) input_state.buttons |= InputFlags::MOVE_LEFT;
            if (input_manager->is_key_held(SDL_SCANCODE_D)) input_state.buttons |= InputFlags::MOVE_RIGHT;
            if (input_manager->is_key_held(SDL_SCANCODE_SPACE)) input_state.buttons |= InputFlags::JUMP;
            if (input_manager->is_key_held(SDL_SCANCODE_E)) input_state.buttons |= InputFlags::USE;

            // Get camera rotation from world camera
            input_state.camera_yaw = _world.world_camera.rotation.y;
            input_state.camera_pitch = _world.world_camera.rotation.x;
            input_state.move_forward = 0.0f;
            input_state.move_right = 0.0f;

            // Calculate analog movement values
            if (input_state.buttons & InputFlags::MOVE_FORWARD) input_state.move_forward += 1.0f;
            if (input_state.buttons & InputFlags::MOVE_BACK) input_state.move_forward -= 1.0f;
            if (input_state.buttons & InputFlags::MOVE_RIGHT) input_state.move_right += 1.0f;
            if (input_state.buttons & InputFlags::MOVE_LEFT) input_state.move_right -= 1.0f;

            _network.sendInputCommand(input_state);
        }

        // physics and player collisions (only when controlling player)
        if (!player_controller->isFreecamMode())
        {
            _world.step_physics();
            _world.player_collisions(player_entity, 1.0f); // Assuming radius 1
        }

        // Update currently possessed entity through player controller
        player_controller->update(_world.fixed_delta);

        // Update player representation visibility and sync
        update_player_representations(_world.registry, player_controller->isFreecamMode());

        // Fall detection (only when controlling player)
        if (!player_controller->isFreecamMode() && _world.registry.valid(player_entity)) {
             auto& t = _world.registry.get<TransformComponent>(player_entity);
             if (t.position.y < -50) // Increased threshold
                quit_game(0);
        }

        // render using the active camera (either player or freecam)
        camera& active_camera = player_controller->getActiveCamera();
        _renderer.render_scene(_world.registry, active_camera);

        app.swapBuffers();

        frame_end_ticks = SDL_GetTicks();

        // Update target FPS from cvar
        int fps_max_val = CVAR_INT(fps_max);
        if (fps_max_val > 0) {
            app.setTargetFPS(fps_max_val);
        } else {
            app.setTargetFPS(10000); // Effectively unlimited
        }
        app.lockFramerate(frame_start_ticks, frame_end_ticks);
    }

    crashHandler->Shutdown();
    exit(0);
}
