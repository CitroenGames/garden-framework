#pragma once

#include "InputManager.hpp"
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

    // Process all SDL events for this frame
    void process_events()
    {
        // Update input manager at start of frame
        input_manager->update();
        
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                should_quit = true;
                if (quit_callback)
                {
                    quit_callback();
                }
                break;
                
            default:
                // Let the input manager handle all other events
                input_manager->process_event(event);
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