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

#include "Application.hpp"
#include "Components/gameObject.hpp"
#include "Components/rigidbody.hpp"
#include "Components/mesh.hpp"
#include "Components/collider.hpp"
#include "Components/playerEntity.hpp"
#include "freecamEntity.hpp"
#include "PlayerController.hpp"
#include "InputHandler.hpp"
#include "Components/playerRepresentation.hpp"
#include "world.hpp"
#include "Graphics/renderer.hpp"
#include "AudioSystem.h"
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

    // Apply textures using the new material system
    bool texture_applied = false;
    
    if (map_result.materials_loaded && !map_result.material_data.materials.empty()) {
        // Try each material until we find one with a valid texture
        for (const auto& material : map_result.material_data.materials) {
            TextureHandle primary_texture = material.getPrimaryTextureHandle();
            
            if (primary_texture != INVALID_TEXTURE) {
                gltf_mesh->set_texture(primary_texture);
                printf("Applied texture from material: %s\n", material.properties.name.c_str());
                texture_applied = true;
                break;
            }
        }

        // Print material information for debugging
        if (material_config.verbose_logging) {
            printf("Available materials:\n");
            for (size_t i = 0; i < map_result.material_data.materials.size(); ++i) {
                const auto& mat = map_result.material_data.materials[i];
                printf("  [%zu] %s - %s\n", i, mat.properties.name.c_str(),
                       mat.hasValidTextures() ? "has textures" : "no textures");
                
                // Show texture details
                for (const auto& tex : mat.textures.textures) {
                    printf("    %s: %s %s\n", 
                           getTextureTypeName(tex.type).c_str(),
                           tex.uri.c_str(),
                           tex.is_loaded ? "(loaded)" : "(failed)");
                }
            }
        }
    }

    // Fallback texture if no materials had valid textures
    if (!texture_applied) {
        printf("No valid textures found in materials, using fallback\n");
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
    TE::AudioSystem audioSystem;
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

    /* Gameobjects */
    gameObject sky = gameObject::gameObject();  // skybox
    gameObject map = gameObject::gameObject();
    gameObject cube = gameObject::gameObject(14, 3.5f, -3.5f); // test cube
    gameObject player = gameObject::gameObject(0, 2, 0);
    gameObject freecam_obj = gameObject::gameObject(0, 2, 0);
    gameObject player_rep_obj = gameObject::gameObject(0, 2, 0); // Player representation object

    /* Cameras */
    camera freecam_camera = camera::camera(0, 2, 0);

    /* Rigidbodies */
    rigidbody player_rb = rigidbody::rigidbody(player);
    player_rb.apply_gravity = false;
    std::vector<rigidbody*> rigidbodies;
    rigidbodies.push_back(&player_rb);

    /* Player and Freecam entities with new input system */
    playerEntity player_entity = playerEntity::playerEntity(_world.world_camera, player_rb, player, input_manager);
    freecamEntity freecam_entity = freecamEntity::freecamEntity(freecam_camera, freecam_obj, input_manager);

    // Set up player controller with new input system
    player_controller = std::make_unique<PlayerController>(input_manager);
    player_controller->setPossessedPlayer(&player_entity);
    player_controller->setPossessedFreecam(&freecam_entity);

    _world.player_entity = &player_entity;

    /* Meshes */
    mesh sky_mesh = mesh("models/sky.obj", sky);

    // Load glTF files
    mesh* map_ground_mesh = loadGltfMeshWithMaterials("models/map.gltf", map, render_api);
    mesh* player_rep_mesh = loadGltfMeshWithMaterials("models/Character.gltf", player_rep_obj, render_api);

    // Continue with other mesh loading...
    mesh map_trees_mesh = mesh::mesh("models/map_trees.obj", map);
    map_trees_mesh.culling = false;
    map_trees_mesh.transparent = true;

    mesh map_bgtrees_mesh = mesh::mesh("models/map_bgtrees.obj", map);
    map_bgtrees_mesh.transparent = true;

    mesh map_collider_mesh = mesh::mesh("models/map_collider.obj", map);

    mesh cube_mesh = mesh::mesh("models/grasscube.obj", cube);

    
    player_rep_obj.scale = vector3f(0.2f, 0.2f, 0.2f);
    player_rep_obj.position = vector3f(0, -20, 0);

    // Create player representation component
    PlayerRepresentation player_representation = PlayerRepresentation(player_rep_mesh, &player_entity, player_rep_obj);

    std::vector<mesh*> meshes;
    meshes.push_back(&sky_mesh);
    if (map_ground_mesh) {
        meshes.push_back(map_ground_mesh);
    }
    meshes.push_back(&cube_mesh);
    meshes.push_back(&map_bgtrees_mesh);
    meshes.push_back(&map_trees_mesh);
    meshes.push_back(player_rep_mesh);

    /* Colliders */
    collider cube_collider = collider::collider(cube_mesh, cube);
    collider map_collider = collider::collider(map_collider_mesh, map);
    std::vector<collider*> colliders;
    colliders.push_back(&cube_collider);
    colliders.push_back(&map_collider);

    /* Textures - Using the abstracted render API */
    TextureHandle sky_tex = render_api->loadTexture("textures/t_sky.png", false, true);
    sky_mesh.set_texture(sky_tex);

    TextureHandle ball_tex = render_api->loadTexture("textures/man.bmp", true, true);
    cube_mesh.set_texture(ball_tex);

    TextureHandle tree_bark = render_api->loadTexture("textures/t_tree_bark.png", true, true);
    TextureHandle tree_leaves = render_api->loadTexture("textures/t_tree_leaves.png", true, true);

    map_trees_mesh.set_texture(tree_bark);
    map_bgtrees_mesh.set_texture(tree_leaves);

    /* Renderer - Using the abstracted render API */
    _renderer = renderer::renderer(&meshes, render_api);

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

    printf("=== INPUT CONTROLS ===\n");
    printf("WASD: Move\n");
    printf("Space: Jump (Player) / Move Up (Freecam)\n");
    printf("Shift: Move Down (Freecam)\n");
    printf("F: Toggle between Player and Freecam mode\n");
    printf("ESC: Quit game\n");
    printf("Mouse: Look around\n");
    printf("=====================\n");

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
            _world.player_collisions(player_rb, 1, colliders);
        }

        // Update currently possessed entity through player controller
        player_controller->update(_world.fixed_delta);

        // Update player representation visibility
        player_representation.update(player_controller->isFreecamMode());

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
    if (map_ground_mesh) {
        delete map_ground_mesh;
    }

    crashHandler->Shutdown();
    exit(0);
}