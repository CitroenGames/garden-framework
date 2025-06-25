#pragma once

#include "SDL.h"
#include "Graphics/RenderAPI.hpp"
#include <stdio.h>
#include <SDL_syswm.h>

class Application
{
private:
    SDL_Window* window;
    IRenderAPI* render_api;
    int width;
    int height;
    int target_fps;
    float fov;
    RenderAPIType api_type;

public:
    Application(int w = 1920, int h = 1080, int fps = 60, float field_of_view = 75.0f, RenderAPIType render_type = RenderAPIType::OpenGL)
        : window(nullptr), render_api(nullptr), width(w), height(h), target_fps(fps), fov(field_of_view), api_type(render_type)
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

        // Create window (platform-agnostic)
        Uint32 window_flags = 0;
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

        // Create render API
        render_api = CreateRenderAPI(api_type);
        if (!render_api)
        {
            fprintf(stderr, "Failed to create render API\n");
            return false;
        }

        // Get platform-specific window handle
        WindowHandle window_handle = getWindowHandle();
        if (!window_handle)
        {
            fprintf(stderr, "Failed to get window handle\n");
            return false;
        }

        // Initialize the render API with the window handle
        if (!render_api->initialize(window_handle, width, height, fov))
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

        if (window)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        SDL_Quit();
    }

    void swapBuffers()
    {
        if (render_api)
        {
            render_api->present();
        }
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
    WindowHandle getWindowHandle()
    {
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info))
        {
            return (WindowHandle)info.info.win.window;
        }
#elif defined(__linux__)
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info))
        {
            return (WindowHandle)info.info.x11.window;
        }
#elif defined(__APPLE__)
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info))
        {
            return (WindowHandle)info.info.cocoa.window;
        }
#endif
        return nullptr;
    }
};