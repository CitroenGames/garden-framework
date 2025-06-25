#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "CrashHandler.hpp"
#include <windows.h>
#endif

#include "math.h"
#include "SDL.h"
#include <GL/gl.h>
#include <GL/glu.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

// Helper function to load textures using stb_image
static GLuint load_texture(const char* filename, bool invert_y = false, bool generate_mipmaps = true)
{
    int width, height, channels;

    // Set flip vertically if needed (equivalent to SOIL_FLAG_INVERT_Y)
    stbi_set_flip_vertically_on_load(invert_y);

    unsigned char* data = stbi_load(filename, &width, &height, &channels, 0);
    if (!data)
    {
        fprintf(stderr, "Failed to load texture: %s\n", filename);
        return 0;
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Determine format based on channels
    GLenum format;
    GLenum internal_format;
    switch (channels)
    {
    case 1:
        format = GL_LUMINANCE;
        internal_format = GL_LUMINANCE;
        break;
    case 3:
        format = GL_RGB;
        internal_format = GL_RGB;
        break;
    case 4:
        format = GL_RGBA;
        internal_format = GL_RGBA;
        break;
    default:
        fprintf(stderr, "Unsupported number of channels: %d\n", channels);
        stbi_image_free(data);
        glDeleteTextures(1, &texture);
        return 0;
    }

    // Generate mipmaps if requested
    if (generate_mipmaps)
    {
        // Use gluBuild2DMipmaps instead of glGenerateMipmap for OpenGL 1.x
        gluBuild2DMipmaps(GL_TEXTURE_2D, internal_format, width, height, format, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        // Upload texture data without mipmaps
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Set texture wrapping
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Free image data
    stbi_image_free(data);

    // Unbind texture
    glBindTexture(GL_TEXTURE_2D, 0);

    return texture;
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

    // Initialize application
    app = Application(1920, 1080, 60, 75.0f);
    if (!app.initialize("Game Window", true))
    {
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

    /* Textures - Updated to use stb_image */
    GLuint sky_tex = load_texture("textures/t_sky.png", false, true);  // no invert_y for sky
    sky_mesh.set_texture(sky_tex);

    GLuint ball_tex = load_texture("textures/man.bmp", true, true);  // invert_y for grasscube
    cube_mesh.set_texture(ball_tex);

    GLuint tree_bark = load_texture("textures/t_tree_bark.png", true, true);
    GLuint tree_leaves = load_texture("textures/t_tree_leaves.png", true, true);
    GLuint groundtexture = load_texture("textures/t_ground.png", true, true);

    map_trees_mesh.set_texture(tree_bark);
    map_bgtrees_mesh.set_texture(tree_leaves);
    map_ground_mesh.set_texture(groundtexture);

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
        app.swapBuffers();

        frame_end_ticks = SDL_GetTicks();
        app.lockFramerate(frame_start_ticks, frame_end_ticks);
    }

    crashHandler->Shutdown();
    exit(0);
}