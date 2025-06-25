#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Utils/CrashHandler.hpp"
#include <windows.h>
#endif

#include "math.h"
#include "SDL.h"

#include <stdio.h>
#include <stdlib.h>

#include "Application.hpp"
#include "gameObject.hpp"
#include "rigidbody.hpp"
#include "mesh.hpp"
#include "collider.hpp"
#include "playerEntity.hpp"
#include "freecamEntity.hpp"
#include "PlayerController.hpp"
#include "playerRepresentation.hpp"
#include "world.hpp"
#include "Graphics/renderer.hpp"
#include "AudioSystem.h"

static Application app;
static renderer _renderer;
static world _world;
static PlayerController player_controller;

static void quit_game(int code)
{
    app.shutdown();
    exit(code);
}

static void handle_key_down(SDL_Keysym* keysym)
{
    switch (keysym->sym)
    {
    case SDLK_ESCAPE:
        quit_game(0);
        break;

    default:
        // Pass to player controller
        player_controller.handleKeyDown(keysym);
        break;
    }
}

static void process_events()
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_MOUSEMOTION:
            player_controller.handleMouseMotion(event.motion.yrel, event.motion.xrel);
            break;

        case SDL_KEYDOWN:
            handle_key_down(&event.key.keysym);
            break;

        case SDL_KEYUP:
            player_controller.handleKeyUp(&event.key.keysym);
            break;

        case SDL_QUIT:
            quit_game(0);
            break;
        }
    }
}

#if _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char* argv[])
#endif
{
    TE::AudioSystem audioSystem;
    Paingine2D::CrashHandler* crashHandler = Paingine2D::CrashHandler::GetInstance();
    crashHandler->Initialize("FNaF");

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
        fprintf(stderr, "Failed to get render API from application\n");
        quit_game(1);
    }

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

    /* Player and Freecam entities */
    playerEntity player_entity = playerEntity::playerEntity(_world.world_camera, player_rb, player);
    freecamEntity freecam_entity = freecamEntity::freecamEntity(freecam_camera, freecam_obj);

    // Set up player controller
    player_controller.setPossessedPlayer(&player_entity);
    player_controller.setPossessedFreecam(&freecam_entity);

    _world.player_entity = &player_entity;

    /* Meshes */
    mesh sky_mesh = mesh("models/sky.obj", sky);

    mesh map_ground_mesh = mesh::mesh("models/map_ground.obj", map);
    mesh map_trees_mesh = mesh::mesh("models/map_trees.obj", map);
    map_trees_mesh.culling = false;
    map_trees_mesh.transparent = true;
    mesh map_bgtrees_mesh = mesh::mesh("models/map_bgtrees.obj", map);
    map_bgtrees_mesh.transparent = true;
    mesh map_collider_mesh = mesh::mesh("models/map_collider.obj", map);

    mesh cube_mesh = mesh::mesh("models/grasscube.obj", cube);

    mesh player_rep_mesh = mesh::mesh("models/player_character.obj", player_rep_obj);
    player_rep_obj.scale = vector3f(0.2f, 0.2f, 0.2f);
    player_rep_obj.position = vector3f(0, -20, 0);

    // Create player representation component
    PlayerRepresentation player_representation = PlayerRepresentation(&player_rep_mesh, &player_entity, player_rep_obj);

    std::vector<mesh*> meshes;
    meshes.push_back(&sky_mesh);
    meshes.push_back(&map_ground_mesh);
    meshes.push_back(&cube_mesh);
    meshes.push_back(&map_bgtrees_mesh);
    meshes.push_back(&map_trees_mesh);
    meshes.push_back(&player_rep_mesh); // Add player representation to render list

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
    TextureHandle groundtexture = render_api->loadTexture("textures/t_ground.png", true, true);

    map_trees_mesh.set_texture(tree_bark);
    map_bgtrees_mesh.set_texture(tree_leaves);
    map_ground_mesh.set_texture(groundtexture);

    // Set texture for player representation (you can use any texture you want)
    TextureHandle player_texture = render_api->loadTexture("textures/player.png", true, true);
    player_rep_mesh.set_texture(player_texture);
    // If you don't have a player texture, you can reuse an existing one:
    // player_rep_mesh.set_texture(ball_tex);

    /* Renderer - Using the abstracted render API */
    _renderer = renderer::renderer(&meshes, render_api);

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

    printf("Game initialized with %s render API\n", render_api->getAPIName());
    printf("Press F to toggle between player and freecam mode\n");

    atexit(SDL_Quit);
    while (1)
    {
        frame_start_ticks = SDL_GetTicks();

        // player input + extra input
        process_events();

        // delta time
        delta_time = (frame_start_ticks - delta_last) / 1000.0f;
        delta_last = frame_start_ticks;

        // physics and player collisions (only when controlling player)
        if (!player_controller.isFreecamMode())
        {
            _world.step_physics(rigidbodies);
            _world.player_collisions(player_rb, 1, colliders);
        }

        // Update currently possessed entity through player controller
        player_controller.update(_world.fixed_delta);

        // Update player representation visibility
        player_representation.update(player_controller.isFreecamMode());

        // Fall detection (only when controlling player)
        if (!player_controller.isFreecamMode() && player_entity.obj.position.Y < -5)
            quit_game(0);

        // render using the active camera (either player or freecam)
        camera& active_camera = player_controller.getActiveCamera();
        _renderer.render_scene(active_camera);
        app.swapBuffers();

        frame_end_ticks = SDL_GetTicks();
        app.lockFramerate(frame_start_ticks, frame_end_ticks);
    }

    crashHandler->Shutdown();
    exit(0);
}