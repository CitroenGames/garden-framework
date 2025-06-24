#define SDL_MAIN_HANDLED

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "math.h"

#include "SDL.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include "SOIL.h"

#include <stdio.h>
#include <stdlib.h>

#include "gameObject.hpp"
#include "rigidbody.hpp"
#include "mesh.hpp"
#include "collider.hpp"

#include "playerEntity.hpp"

#include "models/m_grasscube.hpp"
#include "models/m_sphere.hpp"
#include "models/m_sky.hpp"

#include "models/m_map_ground.hpp"
#include "models/m_map_trees.hpp"
#include "models/m_map_bgtrees.hpp"
#include "models/m_map_collider.hpp"

#include "world.hpp"
#include "renderer.hpp"

static int width = 640;
static int height = 480;
static int target_fps = 60;

static float fov = 75;

static renderer _renderer;
static world _world;

static SDL_Window* window = NULL;
static SDL_GLContext gl_context;

// TODO: abstract this...
static void quit_game(int code)
{
    if (gl_context)
        SDL_GL_DeleteContext(gl_context);
    
    if (window)
        SDL_DestroyWindow(window);
    
    SDL_Quit();

    /* Disable capabilities we enabled before. */
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    /* Exit program. */
    exit(code);
}

static void lock_framerate(Uint32 start, Uint32 end, int target)
{
    int frame_delay = 1000 / target;
    float delta = end - start;

    if (delta < frame_delay)
        SDL_Delay(frame_delay - delta);
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

static void setup_sdl(int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
        quit_game(1);
    }

    /* Set OpenGL attributes */
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* Create window */
    window = SDL_CreateWindow("Game Window",
                             SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             width, height,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);

    if (!window)
    {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        quit_game(1);
    }

    /* Create OpenGL context */
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
    {
        fprintf(stderr, "OpenGL context creation failed: %s\n", SDL_GetError());
        quit_game(1);
    }

    /* Input stuff */
    SDL_SetRelativeMouseMode(SDL_TRUE);
}

static void setup_opengl(int width, int height)
{
    float ratio = (float)width / (float)height;

    /* Our shading model--Gouraud (smooth) */
    glShadeModel(GL_SMOOTH);

    /* Features */
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    /* Depth test */
    //glClearDepth(1.0);
    glDepthFunc(GL_LESS);

    /* Client-side capabilities */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    /* Set the clear color */
    glClearColor(0, 0, 0, 0);

    /* Setup our viewport */
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fov, ratio, 0.1f, 200.0);
}

int main(int argc, char* argv[])
{
    setup_sdl(width, height);
    setup_opengl(width, height);

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
    mesh sky_mesh = mesh::mesh(m_sky_vertices, m_sky_vertices_len, sky);

    mesh map_ground_mesh = mesh::mesh(m_map_ground_vertices, m_map_ground_vertices_len, map);
    mesh map_trees_mesh = mesh::mesh(m_map_trees_vertices, m_map_trees_vertices_len, map);
    map_trees_mesh.culling = false;
    map_trees_mesh.transparent = true;
    mesh map_bgtrees_mesh = mesh::mesh(m_map_bgtrees_vertices, m_map_bgtrees_vertices_len, map);
    map_bgtrees_mesh.transparent = true;
    mesh map_collider_mesh = mesh::mesh(m_map_collider_vertices, m_map_collider_vertices_len, map);

    mesh cube_mesh = mesh::mesh(m_grasscube_vertices, m_grasscube_vertices_len, cube);

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

    /* Textures */
    GLuint sky_tex = SOIL_load_OGL_texture("textures/t_sky.png", SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT);
    sky_mesh.set_texture(sky_tex);
    GLuint ball_tex = SOIL_load_OGL_texture("textures/t_grasscube.png", SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT);
    cube_mesh.set_texture(ball_tex);

    GLuint t00 = SOIL_load_OGL_texture("textures/t00.png", SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT);
    GLuint t01 = SOIL_load_OGL_texture("textures/t01.png", SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT);
    GLuint t02 = SOIL_load_OGL_texture("textures/t02.png", SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT);
    map_trees_mesh.set_texture(t00);
    map_ground_mesh.set_texture(t01);
    map_bgtrees_mesh.set_texture(t01);

    /* Renderer */
    _renderer = renderer::renderer(&meshes);

    /* Delta time */
    Uint32 delta_last = 0;
    float delta_time = 0;

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

        frame_end_ticks = SDL_GetTicks();
        lock_framerate(frame_start_ticks, frame_end_ticks, target_fps);
    }

    exit(0);
}