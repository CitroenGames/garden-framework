#pragma once

#include "InputManager.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "SDL.h"
#include <memory>
#include <functional>

// Global input handler that processes SDL events and manages the InputManager
class InputHandler
{
private:
    std::shared_ptr<InputManager> input_manager;
    std::function<void()> quit_callback;
    bool should_quit = false;
    bool ui_mode = false;  // When true, mouse is visible and game input is paused

public:
    InputHandler() 
    {
        input_manager = std::make_shared<InputManager>();
    }
    
    ~InputHandler() = default;

    // Set a callback for when the application should quit
    void set_quit_callback(std::function<void()> callback)
    {
        quit_callback = callback;
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
            case SDL_QUIT:
                should_quit = true;
                if (quit_callback)
                {
                    quit_callback();
                }
                break;

            case SDL_KEYDOWN:
                // F3 toggles UI mode (mouse visible, game input paused)
                if (event.key.keysym.scancode == SDL_SCANCODE_F3 && !event.key.repeat)
                {
                    ui_mode = !ui_mode;
                    SDL_SetRelativeMouseMode(ui_mode ? SDL_FALSE : SDL_TRUE);
                    break;  // Don't pass F3 to game input
                }
                // Fall through to default handling for other keys
                [[fallthrough]];

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