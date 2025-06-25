#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "CrashHandler.hpp"
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
#include "world.hpp"
#include "renderer.hpp"
#include "AudioSystem.h"

static Application app;
static renderer _renderer;
static world _world;

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
        //quit_game(0);
        break;

    default:
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
            _world.player_entity->update_camera(event.motion.yrel, event.motion.xrel);
            break;

        case SDL_KEYDOWN:
            handle_key_down(&event.key.keysym);
            _world.player_entity->handle_input_down(&event.key.keysym);
            break;

        case SDL_KEYUP:
            _world.player_entity->handle_input_up(&event.key.keysym);
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

    /* Rigidbodies */
    rigidbody player_rb = rigidbody::rigidbody(player);
    player_rb.apply_gravity = false;
    std::vector<rigidbody*> rigidbodies;
    rigidbodies.push_back(&player_rb);

    /* Player */
    playerEntity player_entity = playerEntity::playerEntity(_world.world_camera, player_rb, player);
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

    std::vector<mesh*> meshes;
    meshes.push_back(&sky_mesh);
    meshes.push_back(&map_ground_mesh);
    meshes.push_back(&cube_mesh);
    meshes.push_back(&map_bgtrees_mesh);
    meshes.push_back(&map_trees_mesh);

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

    /* Renderer - Using the abstracted render API */
    _renderer = renderer::renderer(&meshes, render_api);

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

    printf("Game initialized with %s render API\n", render_api->getAPIName());

    atexit(SDL_Quit);
    while (1)
    {
        frame_start_ticks = SDL_GetTicks();

        // player input + extra input
        process_events();

        // delta time
        delta_time = (frame_start_ticks - delta_last) / 1000.0f;
        delta_last = frame_start_ticks;

        // physics and player collisions
        _world.step_physics(rigidbodies);
        _world.player_collisions(player_rb, 1, colliders);

        // player loop
        player_entity.update_player(_world.fixed_delta);

        if (player_entity.obj.position.Y < -5)
            quit_game(0);

        // render
        _renderer.render_scene(_world.world_camera);
        app.swapBuffers();

        frame_end_ticks = SDL_GetTicks();
        app.lockFramerate(frame_start_ticks, frame_end_ticks);
    }

    crashHandler->Shutdown();
    exit(0);
}