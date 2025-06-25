#pragma once

#include "SDL.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>

class Application
{
private:
    SDL_Window* window;
    SDL_GLContext gl_context;
    int width;
    int height;
    int target_fps;
    float fov;

public:
    Application(int w = 1920, int h = 1080, int fps = 60, float field_of_view = 75.0f)
        : window(nullptr), gl_context(nullptr), width(w), height(h), target_fps(fps), fov(field_of_view)
    {
    }

    ~Application()
    {
        shutdown();
    }

    bool initialize(const char* title = "Game Window", bool fullscreen = true)
    {
        if (SDL_Init(SDL_INIT_VIDEO) < 0)
        {
            fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
            return false;
        }

        // Set OpenGL attributes
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        // Create window
        Uint32 window_flags = SDL_WINDOW_OPENGL;
        if (fullscreen)
            window_flags |= SDL_WINDOW_FULLSCREEN;

        window = SDL_CreateWindow(title,
                                 SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED,
                                 width, height,
                                 window_flags);

        if (!window)
        {
            fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
            return false;
        }

        // Create OpenGL context
        gl_context = SDL_GL_CreateContext(window);
        if (!gl_context)
        {
            fprintf(stderr, "OpenGL context creation failed: %s\n", SDL_GetError());
            return false;
        }

        // Input setup
        SDL_SetRelativeMouseMode(SDL_TRUE);

        // Initialize OpenGL
        setupOpenGL();

        return true;
    }

    void shutdown()
    {
        // Disable OpenGL capabilities
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);

        if (gl_context)
        {
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
        }

        if (window)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        SDL_Quit();
    }

    void swapBuffers()
    {
        SDL_GL_SwapWindow(window);
    }

    void lockFramerate(Uint32 start_time, Uint32 end_time)
    {
        int frame_delay = 1000 / target_fps;
        float delta = end_time - start_time;

        if (delta < frame_delay)
            SDL_Delay(frame_delay - delta);
    }

    // Getters
    SDL_Window* getWindow() const { return window; }
    SDL_GLContext getGLContext() const { return gl_context; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getTargetFPS() const { return target_fps; }
    float getFOV() const { return fov; }

    // Setters
    void setTargetFPS(int fps) { target_fps = fps; }
    void setFOV(float field_of_view) { fov = field_of_view; }

private:
    void setupOpenGL()
    {
        float ratio = (float)width / (float)height;

        // Enable depth testing
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glClearDepth(1.0);

        // Enable face culling
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // Shading model
        glShadeModel(GL_SMOOTH);

        // Enable vertex arrays
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        // Set clear color (light blue)
        glClearColor(0.2f, 0.3f, 0.8f, 1.0f);

        // Set up projection matrix
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(fov, ratio, 0.1, 200.0);

        // Set up viewport
        glViewport(0, 0, width, height);

        // Switch to modelview matrix
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Enable color material for easy color changes
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    }
};