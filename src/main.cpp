#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "Utils/CrashHandler.hpp"
#include <windows.h>
#endif

#include "math.h"
#include "SDL.h"

#include <stdio.h>
#include <stdlib.h>
#include <map>

#include "Application.hpp"

// Components
#include "Components/gameObject.hpp"
#include "Components/rigidbody.hpp"
#include "Components/mesh.hpp"
#include "Components/collider.hpp"
#include "Components/playerEntity.hpp"
#include "Components/FreecamEntity.hpp"

#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "Components/playerRepresentation.hpp"
#include "world.hpp"
#include "Graphics/renderer.hpp"
#include "LevelManager.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/GltfMaterialLoader.hpp"

#include "Utils/Log.hpp"

static Application app;
static renderer _renderer;
static world _world;
static InputHandler input_handler;
static std::unique_ptr<PlayerController> player_controller;

// TODO: move this to a better location
static void quit_game(int code)
{
    app.shutdown();
    exit(code);
}

// TODO: move this to a better location
// Helper function to get texture type name for logging
std::string getTextureTypeName(TextureType type) {
    switch (type) {
        case TextureType::BASE_COLOR: return "Base Color";
        case TextureType::METALLIC_ROUGHNESS: return "Metallic-Roughness";
        case TextureType::NORMAL: return "Normal";
        case TextureType::OCCLUSION: return "Occlusion";
        case TextureType::EMISSIVE: return "Emissive";
        case TextureType::DIFFUSE: return "Diffuse";
        case TextureType::SPECULAR: return "Specular";
        default: return "Unknown";
    }
}

// TODO: move this to a better location
// Function to load glTF
mesh* loadGltfMeshWithMaterials(const std::string& filename, gameObject& obj, IRenderAPI* render_api)
{
    // Configure geometry loading
    GltfLoaderConfig gltf_config;
    gltf_config.verbose_logging = true;
    gltf_config.flip_uvs = true;
    gltf_config.generate_normals_if_missing = true;
    gltf_config.scale = 1.0f;

    // Configure material loading with performance optimizations
    MaterialLoaderConfig material_config;
    material_config.verbose_logging = true;
    material_config.load_all_textures = false;  // Only load essential textures for better performance
    material_config.priority_texture_types = {
        TextureType::BASE_COLOR,
        TextureType::DIFFUSE,
        TextureType::NORMAL
    };
    material_config.generate_mipmaps = true;
    material_config.flip_textures_vertically = true;
    material_config.cache_textures = true;  // Prevent loading duplicate textures
    material_config.texture_base_path = "models/";

    // Load geometry and materials together
    GltfLoadResult map_result = GltfLoader::loadGltfWithMaterials(filename, render_api, gltf_config, material_config);

    if (!map_result.success) {
        LOG_ENGINE_FATAL("Failed to load glTF file: {}", map_result.error_message.c_str());
        return nullptr;
    }

    LOG_ENGINE_TRACE("Loaded glTF: {}", filename.c_str());
    LOG_ENGINE_TRACE("Geometry loaded with {} vertices", map_result.vertex_count);
    
    if (map_result.materials_loaded) {
		LOG_ENGINE_TRACE("Materials loaded: {}", map_result.material_data.total_materials);
		LOG_ENGINE_TRACE("- Textures: {} loaded successfully, {} failed",
            map_result.material_data.total_textures_loaded,
            map_result.material_data.total_textures_failed);
    }

    // Create mesh from glTF data
    mesh* gltf_mesh = new mesh(map_result.vertices, map_result.vertex_count, obj);

    // Apply textures using the multi-material system
    bool texture_applied = false;

    if (map_result.materials_loaded && !map_result.material_data.materials.empty()) {
        // Build material ranges from primitive data
        if (!map_result.material_indices.empty() && !map_result.primitive_vertex_counts.empty()) {
            LOG_ENGINE_INFO("Building material ranges for GLTF with {} primitives", map_result.material_indices.size());

            std::vector<MaterialRange> material_ranges;
            size_t current_vertex = 0;

            for (size_t i = 0; i < map_result.material_indices.size(); ++i) {
                int mat_idx = map_result.material_indices[i];
                size_t vertex_count = map_result.primitive_vertex_counts[i];

                if (mat_idx >= 0 && mat_idx < map_result.material_data.materials.size()) {
                    const auto& material = map_result.material_data.materials[mat_idx];
                    TextureHandle tex = material.getPrimaryTextureHandle();

                    // Create a material range for this primitive
                    MaterialRange range(current_vertex, vertex_count, tex, material.properties.name);
                    material_ranges.push_back(range);

                    LOG_ENGINE_TRACE("  Primitive {}: vertices {}-{}, material [{}] '{}'",
                                    i, current_vertex, current_vertex + vertex_count - 1,
                                    mat_idx, material.properties.name);

                    if (tex != INVALID_TEXTURE) {
                        texture_applied = true;
                    }
                }
                else {
                    // Material index out of range, create range with no texture
                    MaterialRange range(current_vertex, vertex_count, INVALID_TEXTURE, "unknown");
                    material_ranges.push_back(range);

                    LOG_ENGINE_WARN("  Primitive {}: invalid material index [{}]", i, mat_idx);
                }

                current_vertex += vertex_count;
            }

            // Set the material ranges on the mesh
            if (!material_ranges.empty()) {
                gltf_mesh->setMaterialRanges(material_ranges);
                LOG_ENGINE_INFO("Applied {} material ranges to mesh", material_ranges.size());
            }
        }

        // Print material information for debugging
        if (material_config.verbose_logging) {
            LOG_ENGINE_TRACE("Available materials:");
            for (size_t i = 0; i < map_result.material_data.materials.size(); ++i) {
                const auto& mat = map_result.material_data.materials[i];
                LOG_ENGINE_TRACE("  [{}] {} - {}", i, mat.properties.name,
                       mat.hasValidTextures() ? "has textures" : "no textures");

                // Show texture details
                for (const auto& tex : mat.textures.textures) {
                    LOG_ENGINE_TRACE("    {}: {} {}",
                           getTextureTypeName(tex.type),
                           tex.uri,
                           tex.is_loaded ? "(loaded)" : "(failed)");
                }
            }
        }
    }

    // Fallback texture if no materials had valid textures
    if (!texture_applied) {
        LOG_ENGINE_WARN("No valid textures found in materials, using fallback");
        TextureHandle fallback_texture = render_api->loadTexture("textures/t_ground.png", true, true);
        gltf_mesh->set_texture(fallback_texture);
    }

    // Transfer ownership to prevent cleanup
    map_result.vertices = nullptr;
    map_result.vertex_count = 0;

    return gltf_mesh;
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

    // Initialize application with OpenGL render API
    app = Application(1920, 1080, 60, 75.0f, RenderAPIType::OpenGL);
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

    // Containers for runtime objects
    std::vector<mesh*> meshes;
    std::vector<rigidbody*> rigidbodies;
    std::vector<collider*> colliders;
    LevelEntity* player_data = nullptr;
    LevelEntity* freecam_data = nullptr;
    LevelEntity* player_rep_data = nullptr;

    // Instantiate level entities
    if (!level_manager.instantiateLevel(
            level_data, _world, render_api, meshes, rigidbodies, colliders,
            &player_data, &freecam_data, &player_rep_data))
    {
        LOG_ENGINE_FATAL("Failed to instantiate level");
        quit_game(1);
    }

    // Verify critical entities exist
    if (!player_data)
    {
        LOG_ENGINE_FATAL("Level does not contain a player entity");
        quit_game(1);
    }

    if (!player_data->game_object || !player_data->rigidbody_component)
    {
        LOG_ENGINE_FATAL("Player entity is missing required components");
        quit_game(1);
    }

    // Create freecam camera
    camera freecam_camera = camera::camera(
        freecam_data ? freecam_data->position.X : 0,
        freecam_data ? freecam_data->position.Y : 2,
        freecam_data ? freecam_data->position.Z : 0
    );

    // Create player and freecam entities with input manager
    playerEntity player_entity = playerEntity::playerEntity(
        _world.world_camera,
        *player_data->rigidbody_component,
        *player_data->game_object,
        input_manager
    );

    // Set player properties from level data
    player_entity.speed = player_data->speed;
    player_entity.jump_force = player_data->jump_force;
    player_entity.mouse_sensitivity = player_data->mouse_sensitivity;

    freecamEntity freecam_entity = freecamEntity::freecamEntity(
        freecam_camera,
        *freecam_data->game_object,
        input_manager
    );

    // Set freecam properties from level data
    freecam_entity.movement_speed = freecam_data->movement_speed;
    freecam_entity.fast_movement_speed = freecam_data->fast_movement_speed;
    freecam_entity.mouse_sensitivity = freecam_data->mouse_sensitivity;

    // Set up player controller with new input system
    player_controller = std::make_unique<PlayerController>(input_manager);
    player_controller->setPossessedPlayer(&player_entity);
    player_controller->setPossessedFreecam(&freecam_entity);

    _world.player_entity = &player_entity;

    // Create player representation if it exists in level
    PlayerRepresentation* player_representation = nullptr;
    if (player_rep_data && player_rep_data->mesh_component)
    {
        player_representation = new PlayerRepresentation(
            player_rep_data->mesh_component,
            &player_entity,
            *player_rep_data->game_object
        );
    }

    /* Renderer - Using the abstracted render API */
    _renderer = renderer::renderer(&meshes, render_api);

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

    atexit(SDL_Quit);
    while (1)
    {
        frame_start_ticks = SDL_GetTicks();

        // Process input events through the new input system
        input_handler.process_events();
        
        // Handle mouse motion for camera control
        if (input_manager)
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

        // delta time
        delta_time = (frame_start_ticks - delta_last) / 1000.0f;
        delta_last = frame_start_ticks;

        // physics and player collisions (only when controlling player)
        if (!player_controller->isFreecamMode())
        {
            _world.step_physics(rigidbodies);
            _world.player_collisions(*player_data->rigidbody_component, 1, colliders);
        }

        // Update currently possessed entity through player controller
        player_controller->update(_world.fixed_delta);

        // Update player representation visibility
        if (player_representation)
        {
            player_representation->update(player_controller->isFreecamMode());
        }

        // Fall detection (only when controlling player)
        if (!player_controller->isFreecamMode() && player_entity.obj.position.Y < -5)
            quit_game(0);

        // render using the active camera (either player or freecam)
        camera& active_camera = player_controller->getActiveCamera();
        _renderer.render_scene(active_camera);
        app.swapBuffers();

        frame_end_ticks = SDL_GetTicks();
        app.lockFramerate(frame_start_ticks, frame_end_ticks);
    }

    // Cleanup
    if (player_representation)
    {
        delete player_representation;
    }
    // Level manager cleanup handled by destructor

    crashHandler->Shutdown();
    exit(0);
}