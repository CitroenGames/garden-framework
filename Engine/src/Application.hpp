#pragma once

#include <SDL3/SDL.h>
#include "Graphics/RenderAPI.hpp"
#include "Utils/Log.hpp"
#include <memory>

class Application
{
private:
    SDL_Window* window;
    std::unique_ptr<IRenderAPI> render_api;
    int width;
    int height;
    int target_fps;
    float fov;
    RenderAPIType api_type;

public:
    Application(int w = 1920, int h = 1080, int fps = 60, float field_of_view = 75.0f, RenderAPIType render_type = DefaultRenderAPI)
        : window(nullptr), render_api(nullptr), width(w), height(h), target_fps(fps), fov(field_of_view), api_type(render_type)
    {
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = default;
    Application& operator=(Application&&) = default;

    ~Application()
    {
        shutdown();
    }

    bool initialize(const char* title = "Game Window", bool fullscreen = true)
    {
        if (api_type != RenderAPIType::Headless)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                LOG_ENGINE_FATAL("SDL video initialization failed: {}", SDL_GetError());
                return false;
            }

            SDL_WindowFlags window_flags = 0;
            if (fullscreen)
                window_flags |= SDL_WINDOW_FULLSCREEN;
            else
                window_flags |= SDL_WINDOW_RESIZABLE;

            if (api_type == RenderAPIType::Vulkan)
            {
                // Vulkan requires SDL_WINDOW_VULKAN flag
                window_flags |= SDL_WINDOW_VULKAN;
            }
            else if (api_type == RenderAPIType::Metal)
            {
                // Metal uses CAMetalLayer attached to the native window
                window_flags |= SDL_WINDOW_METAL;
            }
            else if (api_type == RenderAPIType::D3D11 || api_type == RenderAPIType::D3D12)
            {
                // D3D11/D3D12 don't require any special SDL flags
                // The window will be created as a regular Win32 window
            }

            window = SDL_CreateWindow(title, width, height, window_flags);
            if (window)
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

            if (!window)
            {
                LOG_ENGINE_FATAL("Window creation failed: {}", SDL_GetError());
                return false;
            }

            // Prevent degenerate resizes (0-dimension surfaces crash GPU APIs)
            SDL_SetWindowMinimumSize(window, 320, 240);
        }
        else
        {
            // For headless mode, we might still want to initialize SDL but not VIDEO
            if (!SDL_Init(SDL_INIT_EVENTS))
            {
                LOG_ENGINE_FATAL("SDL initialization failed: {}", SDL_GetError());
                return false;
            }
        }

        // Create render API
        render_api.reset(CreateRenderAPI(api_type));
        if (!render_api)
        {
            LOG_ENGINE_FATAL("Failed to create render API (type={})", static_cast<int>(api_type));
            return false;
        }

        LOG_ENGINE_INFO("Created render API: {}", render_api->getAPIName());

        // Initialize the render API with the SDL window pointer (might be null for headless)
        if (!render_api->initialize(window, width, height, fov))
        {
            LOG_ENGINE_FATAL("Failed to initialize render API");
            return false;
        }

        // Input setup
        if (window)
        {
            SDL_SetWindowRelativeMouseMode(window, true);
        }

        LOG_ENGINE_INFO("Application initialized with {} render API", render_api->getAPIName());
        return true;
    }

    void shutdown()
    {
        if (render_api)
        {
            render_api->shutdown();
            render_api.reset();
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

    void lockFramerate(Uint64 start_time, Uint64 end_time)
    {
        Uint64 frame_delay = 1000 / target_fps;
        Uint64 delta = end_time - start_time;

        if (delta < frame_delay)
            SDL_Delay(static_cast<Uint32>(frame_delay - delta));
    }

    // Getters
    SDL_Window* getWindow() const { return window; }
    IRenderAPI* getRenderAPI() const { return render_api.get(); }
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

    // Called when the OS window is resized (by user drag, maximize, etc.)
    void onWindowResized(int new_width, int new_height)
    {
        if (new_width <= 0 || new_height <= 0)
            return;
        width = new_width;
        height = new_height;
        if (render_api)
        {
            render_api->resize(width, height);
        }
    }
};