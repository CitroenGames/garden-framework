#pragma once

#include "SDL.h"
#include "RenderAPI.hpp"
#include <stdio.h>

class Application
{
private:
    SDL_Window* window;
    SDL_GLContext gl_context;
    IRenderAPI* render_api;
    int width;
    int height;
    int target_fps;
    float fov;
    RenderAPIType api_type;

public:
    Application(int w = 1920, int h = 1080, int fps = 60, float field_of_view = 75.0f, RenderAPIType render_type = RenderAPIType::OpenGL)
        : window(nullptr), gl_context(nullptr), render_api(nullptr), width(w), height(h), target_fps(fps), fov(field_of_view), api_type(render_type)
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

        // Create render API
        render_api = CreateRenderAPI(api_type);
        if (!render_api)
        {
            fprintf(stderr, "Failed to create render API\n");
            return false;
        }

        // For OpenGL, we still need to setup SDL OpenGL context
        if (api_type == RenderAPIType::OpenGL)
        {
            if (!initializeOpenGL(title, fullscreen))
                return false;
        }
        // Future: Add initialization for other APIs (Vulkan, DirectX, etc.)

        // Initialize the render API
        if (!render_api->initialize(width, height, fov))
        {
            fprintf(stderr, "Failed to initialize render API\n");
            return false;
        }

        // Input setup
        SDL_SetRelativeMouseMode(SDL_TRUE);

        printf("Application initialized with %s render API\n", render_api->getAPIName());
        return true;
    }

    void shutdown()
    {
        if (render_api)
        {
            render_api->shutdown();
            delete render_api;
            render_api = nullptr;
        }

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
        // For OpenGL, use SDL buffer swap
        if (api_type == RenderAPIType::OpenGL)
        {
            SDL_GL_SwapWindow(window);
        }
        // Future: Handle buffer swapping for other APIs
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
    IRenderAPI* getRenderAPI() const { return render_api; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getTargetFPS() const { return target_fps; }
    float getFOV() const { return fov; }
    RenderAPIType getAPIType() const { return api_type; }

    // Setters
    void setTargetFPS(int fps) { target_fps = fps; }
    void setFOV(float field_of_view) 
    { 
        fov = field_of_view; 
        if (render_api)
        {
            render_api->resize(width, height); // Refresh projection with new FOV
        }
    }

private:
    bool initializeOpenGL(const char* title, bool fullscreen)
    {
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

        return true;
    }
};