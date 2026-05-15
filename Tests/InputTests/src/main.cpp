#include "InputManager.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <vector>

namespace
{
    SDL_Event makeKeyEvent(Uint32 type, SDL_Scancode scancode, bool repeat = false)
    {
        SDL_Event event{};
        event.type = type;
        event.key.scancode = scancode;
        event.key.repeat = repeat;
        return event;
    }

    SDL_Event makeMouseButtonEvent(Uint32 type, uint8_t button)
    {
        SDL_Event event{};
        event.type = type;
        event.button.button = button;
        return event;
    }

    SDL_Event makeMouseMotionEvent(float xrel, float yrel)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.xrel = xrel;
        event.motion.yrel = yrel;
        return event;
    }

    bool expect(bool condition, const char* message)
    {
        if (!condition)
            std::fprintf(stderr, "FAILED: %s\n", message);
        return condition;
    }

    bool testKeyTransitions()
    {
        InputManager input;
        input.clear_all_mappings();
        input.update();

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));

        bool ok = true;
        ok = expect(input.is_key_pressed(SDL_SCANCODE_SPACE), "key down is pressed for one frame") && ok;
        ok = expect(input.is_key_held(SDL_SCANCODE_SPACE), "key down is held") && ok;
        ok = expect(!input.is_key_released(SDL_SCANCODE_SPACE), "key down is not released") && ok;

        input.update();

        ok = expect(!input.is_key_pressed(SDL_SCANCODE_SPACE), "held key is not pressed next frame") && ok;
        ok = expect(input.is_key_held(SDL_SCANCODE_SPACE), "held key remains held next frame") && ok;

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, true));

        ok = expect(!input.is_key_pressed(SDL_SCANCODE_SPACE), "repeat key down does not retrigger pressed") && ok;

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));

        ok = expect(input.is_key_released(SDL_SCANCODE_SPACE), "key up is released for one frame") && ok;
        ok = expect(!input.is_key_held(SDL_SCANCODE_SPACE), "key up clears held state") && ok;

        input.update();

        ok = expect(!input.is_key_released(SDL_SCANCODE_SPACE), "released key clears next frame") && ok;
        ok = expect(!input.is_key_pressed(SDL_SCANCODE_UNKNOWN), "invalid scancode is ignored") && ok;

        return ok;
    }

    bool testMouseTransitions()
    {
        InputManager input;
        input.clear_all_mappings();
        input.update();

        input.process_event(makeMouseMotionEvent(3.0f, -2.0f));
        input.process_event(makeMouseMotionEvent(1.5f, 4.0f));

        bool ok = true;
        ok = expect(input.get_mouse_delta_x() == 4.5f, "mouse x deltas accumulate") && ok;
        ok = expect(input.get_mouse_delta_y() == 2.0f, "mouse y deltas accumulate") && ok;

        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));

        ok = expect(input.is_mouse_button_pressed(SDL_BUTTON_LEFT), "mouse button down is pressed") && ok;
        ok = expect(input.is_mouse_button_held(SDL_BUTTON_LEFT), "mouse button down is held") && ok;

        input.update();

        ok = expect(input.get_mouse_delta_x() == 0.0f, "mouse x delta resets on update") && ok;
        ok = expect(input.get_mouse_delta_y() == 0.0f, "mouse y delta resets on update") && ok;
        ok = expect(!input.is_mouse_button_pressed(SDL_BUTTON_LEFT), "held mouse button is not pressed next frame") && ok;
        ok = expect(input.is_mouse_button_held(SDL_BUTTON_LEFT), "held mouse button remains held next frame") && ok;

        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));

        ok = expect(input.is_mouse_button_released(SDL_BUTTON_LEFT), "mouse button up is released") && ok;
        ok = expect(!input.is_mouse_button_held(SDL_BUTTON_LEFT), "mouse button up clears held state") && ok;

        return ok;
    }

    bool testActionDispatch()
    {
        InputManager input;
        input.clear_all_mappings();
        input.add_action_mapping("Jump", SDL_SCANCODE_SPACE);

        std::vector<InputActionState> states;
        input.bind_action("Jump", [&states](InputActionState state) {
            states.push_back(state);
        });

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, true));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));

        bool ok = true;
        ok = expect(states.size() == 2, "action dispatches only transition events") && ok;
        if (states.size() == 2)
        {
            ok = expect(states[0] == InputActionState::Pressed, "action dispatches pressed first") && ok;
            ok = expect(states[1] == InputActionState::Released, "action dispatches released second") && ok;
        }

        return ok;
    }

    bool testResetState()
    {
        InputManager input;
        input.clear_all_mappings();
        input.update();

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT));
        input.process_event(makeMouseMotionEvent(8.0f, 6.0f));

        input.reset_state();

        bool ok = true;
        ok = expect(!input.is_key_pressed(SDL_SCANCODE_W), "reset clears key pressed edge") && ok;
        ok = expect(!input.is_key_held(SDL_SCANCODE_W), "reset clears held key") && ok;
        ok = expect(!input.is_mouse_button_pressed(SDL_BUTTON_RIGHT), "reset clears mouse pressed edge") && ok;
        ok = expect(!input.is_mouse_button_held(SDL_BUTTON_RIGHT), "reset clears held mouse button") && ok;
        ok = expect(input.get_mouse_delta_x() == 0.0f, "reset clears mouse x delta") && ok;
        ok = expect(input.get_mouse_delta_y() == 0.0f, "reset clears mouse y delta") && ok;

        return ok;
    }
}

int main()
{
    bool ok = true;
    ok = testKeyTransitions() && ok;
    ok = testMouseTransitions() && ok;
    ok = testActionDispatch() && ok;
    ok = testResetState() && ok;

    if (!ok)
        return 1;

    std::printf("InputTests passed\n");
    return 0;
}
