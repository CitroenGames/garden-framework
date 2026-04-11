#pragma once

#include "InputManager.hpp"
#include "ImGui/ImGuiManager.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <functional>

// Global input handler that processes SDL events and manages the InputManager
class InputHandler
{
private:
    std::shared_ptr<InputManager> input_manager;
    std::function<void()> quit_callback;
    std::function<void(int, int)> resize_callback;
    SDL_Window* window = nullptr;
    bool should_quit = false;
    bool ui_mode = false;  // When true, mouse is visible and game input is paused
    bool is_minimized_state = false;

public:
    InputHandler()
    {
        input_manager = std::make_shared<InputManager>();
    }

    ~InputHandler() = default;

    // Set the SDL window pointer (needed for SDL3 per-window mouse mode)
    void set_window(SDL_Window* w)
    {
        window = w;
    }

    // Set a callback for when the application should quit
    void set_quit_callback(std::function<void()> callback)
    {
        quit_callback = callback;
    }

    // Set a callback for when the window is resized
    void set_resize_callback(std::function<void(int, int)> callback)
    {
        resize_callback = callback;
    }

    // Get the shared input manager
    std::shared_ptr<InputManager> get_input_manager() const
    {
        return input_manager;
    }

    // Check if quit was requested
    bool should_quit_application() const
    {
        return should_quit;
    }

    // Check if UI mode is active (mouse visible, game input paused)
    bool is_ui_mode() const
    {
        return ui_mode;
    }

    // Check if the window is currently minimized
    bool is_window_minimized() const
    {
        return is_minimized_state;
    }

    // Process all SDL events for this frame
    void process_events()
    {
        // Update input manager at start of frame
        input_manager->update();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // Let ImGui process events first
            ImGuiManager::get().processEvent(&event);

            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                should_quit = true;
                if (quit_callback)
                {
                    quit_callback();
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                // Backtick (`) toggles console
                if (event.key.scancode == SDL_SCANCODE_GRAVE && !event.key.repeat)
                {
                    ImGuiManager::get().toggleConsole();
                    // If console is now open, enable UI mode
                    if (ImGuiManager::get().getShowConsole() && !ui_mode)
                    {
                        ui_mode = true;
                        SDL_SetWindowRelativeMouseMode(window, false);
                    }
                    break;  // Don't pass backtick to game input
                }
                // Escape closes console if open
                if (event.key.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat)
                {
                    if (ImGuiManager::get().getShowConsole())
                    {
                        ImGuiManager::get().setShowConsole(false);
                        // Return to game mode if settings panel not shown
                        if (!ImGuiManager::get().getShowSettings())
                        {
                            ui_mode = false;
                            SDL_SetWindowRelativeMouseMode(window, true);
                        }
                        break;  // Don't pass Escape to game input
                    }
                }
                // F3 toggles UI mode (mouse visible, game input paused)
                if (event.key.scancode == SDL_SCANCODE_F3 && !event.key.repeat)
                {
                    ui_mode = !ui_mode;
                    SDL_SetWindowRelativeMouseMode(window, !ui_mode);
                    ImGuiManager::get().setShowSettings(ui_mode);
                    break;  // Don't pass F3 to game input
                }
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                is_minimized_state = true;
                break;

            case SDL_EVENT_WINDOW_RESTORED:
                is_minimized_state = false;
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (resize_callback && window && event.window.windowID == SDL_GetWindowID(window))
                    resize_callback(event.window.data1, event.window.data2);
                break;

            default:
                // Only pass to game input if not in UI mode and ImGui doesn't want input
                if (!ui_mode &&
                    !ImGuiManager::get().wantCaptureMouse() &&
                    !ImGuiManager::get().wantCaptureKeyboard())
                {
                    input_manager->process_event(event);
                }
                break;
            }
        }
    }

    // Reset quit state (useful for handling quit gracefully)
    void reset_quit_state()
    {
        should_quit = false;
    }
};