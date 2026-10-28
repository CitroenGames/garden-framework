#pragma once

#include "InputManager.hpp"
#include "Input/GameInputModeService.hpp"
#include "ImGui/ImGuiManager.hpp"
#include "UI/RmlUiManager.h"
#include <SDL3/SDL.h>
#include <memory>
#include <functional>

// Global input handler that processes SDL events and manages the InputManager
class InputHandler
    : public IGameInputModeService
{
private:
    std::shared_ptr<InputManager> input_manager;
    std::function<void()> quit_callback;
    std::function<void(int, int)> resize_callback;
    SDL_Window* window = nullptr;
    bool should_quit = false;
    bool ui_mode = false;  // When true, mouse is visible and game input is paused
    bool is_minimized_state = false;
    GameInputModeState game_input_mode;

    void set_relative_mouse_mode(bool enabled)
    {
        if (window)
            SDL_SetWindowRelativeMouseMode(window, enabled);
    }

    void refresh_gamepad_input_enabled()
    {
        input_manager->set_gamepad_input_enabled(
            game_input_mode.gameplayInputEnabled() && !ImGuiManager::get().wantCaptureKeyboard());
    }

    void set_ui_mode(bool enabled)
    {
        if (ui_mode == enabled)
            return;

        ui_mode = enabled;
        game_input_mode.setHostUiActive(ui_mode);
        refresh_gamepad_input_enabled();
    }

    bool should_route_to_game_input(const SDL_Event& event) const
    {
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            return game_input_mode.keyboardInputEnabled() &&
                   !ImGuiManager::get().wantCaptureKeyboard();

        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return game_input_mode.gameplayInputEnabled() &&
                   !ImGuiManager::get().wantCaptureMouse();

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return game_input_mode.gameplayInputEnabled() &&
                   !ImGuiManager::get().wantCaptureKeyboard();

        default:
            return false;
        }
    }

    void request_quit()
    {
        should_quit = true;
        if (quit_callback)
            quit_callback();
    }

    bool handle_key_down(const SDL_KeyboardEvent& key)
    {
        if (key.repeat)
            return false;

        // Backtick (`) toggles console.
        if (key.scancode == SDL_SCANCODE_GRAVE)
        {
            ImGuiManager::get().toggleConsole();
            set_ui_mode(ImGuiManager::get().getShowConsole() || ImGuiManager::get().getShowSettings());
            return true;
        }

        // Escape closes console if open.
        if (key.scancode == SDL_SCANCODE_ESCAPE && ImGuiManager::get().getShowConsole())
        {
            ImGuiManager::get().setShowConsole(false);
            set_ui_mode(ImGuiManager::get().getShowSettings());
            return true;
        }

        // F3 toggles UI mode (mouse visible, game input paused).
        if (key.scancode == SDL_SCANCODE_F3)
        {
            const bool next_ui_mode = !ui_mode;
            ImGuiManager::get().setShowSettings(next_ui_mode);
            set_ui_mode(next_ui_mode);
            return true;
        }

        return false;
    }

public:
    InputHandler()
        : input_manager(std::make_shared<InputManager>())
        , game_input_mode(*input_manager, [this](bool gameplay_pointer_capture) {
            set_relative_mouse_mode(gameplay_pointer_capture);
            refresh_gamepad_input_enabled();
        })
    {
    }

    ~InputHandler() = default;

    // Set the SDL window pointer (needed for SDL3 per-window mouse mode)
    void set_window(SDL_Window* w)
    {
        window = w;
        input_manager->initialize_gamepads();
        game_input_mode.synchronize();
    }

    bool requestGameInputMode(GameInputMode mode) override
    {
        return game_input_mode.request(mode);
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
        refresh_gamepad_input_enabled();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // Let ImGui process events first
            ImGuiManager::get().processEvent(&event);

            bool consumed = false;

            // Device topology is tracked even while gameplay input is focused
            // on a menu or the editor UI.
            if (event.type == SDL_EVENT_GAMEPAD_ADDED ||
                event.type == SDL_EVENT_GAMEPAD_REMOVED ||
                event.type == SDL_EVENT_GAMEPAD_REMAPPED)
            {
                input_manager->process_event(event);
                consumed = true;
            }

            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                request_quit();
                consumed = true;
                break;

            case SDL_EVENT_KEY_DOWN:
                consumed = handle_key_down(event.key);
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                is_minimized_state = true;
                consumed = true;
                break;

            case SDL_EVENT_WINDOW_RESTORED:
                is_minimized_state = false;
                consumed = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                game_input_mode.setWindowFocused(false);
                consumed = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                game_input_mode.setWindowFocused(true);
                consumed = true;
                break;

            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (resize_callback && window && event.window.windowID == SDL_GetWindowID(window))
                    resize_callback(event.window.data1, event.window.data2);
                consumed = true;
                break;
            }

            // UI-pointer mode keeps raw keyboard delivery available to the
            // module's keyboard-first modal commands.  Rml still receives
            // the same event afterwards, preserving its focus/text routing.
            const bool route_keyboard_before_rml =
                !consumed &&
                game_input_mode.requestedMode() == GameInputMode::UiPointer &&
                (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                should_route_to_game_input(event);
            if (route_keyboard_before_rml)
                input_manager->process_event(event);

            if (!consumed && RmlUiManager::get().isInitialized())
                consumed = RmlUiManager::get().processEvent(event);

            if (!route_keyboard_before_rml && !consumed && should_route_to_game_input(event))
                input_manager->process_event(event);
        }

        refresh_gamepad_input_enabled();
    }

    // Reset quit state (useful for handling quit gracefully)
    void reset_quit_state()
    {
        should_quit = false;
    }
};
