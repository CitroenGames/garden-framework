#include "InputManager.hpp"
#include "InputActionSystem.hpp"
#include "Components/LocalPlayerInputComponent.hpp"
#include "LocalPlayerInputSystem.hpp"
#include "NetworkInputSampler.hpp"
#include "Network/PredictionTypes.hpp"
#include "json.hpp"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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

    SDL_JoystickID attachVirtualGamepad(const char* name)
    {
        SDL_VirtualJoystickDesc description{};
        SDL_INIT_INTERFACE(&description);
        description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        description.naxes = SDL_GAMEPAD_AXIS_COUNT;
        description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        description.button_mask = (uint32_t{ 1 } << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        description.axis_mask = (uint32_t{ 1 } << SDL_GAMEPAD_AXIS_COUNT) - 1;
        description.name = name;
        const SDL_JoystickID instance_id = SDL_AttachVirtualJoystick(&description);
        if (instance_id == 0)
            return instance_id;

        SDL_Joystick* joystick = SDL_OpenJoystick(instance_id);
        if (joystick)
        {
            SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
            SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
            SDL_CloseJoystick(joystick);
        }
        return instance_id;
    }

    void pumpInputEvents(InputManager& input)
    {
        SDL_PumpEvents();
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            input.process_event(event);
    }

    int firstFreeGamepadSlot(const InputManager& input)
    {
        for (int slot = 0; slot < static_cast<int>(InputGamepadSlotCount); ++slot)
        {
            if (!input.gamepad(slot).connected)
                return slot;
        }
        return -1;
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
        bool saw_key_release = false;
        bool saw_mouse_release = false;
        for (const RawInputTransition& transition : input.get_button_transitions())
        {
            if (!transition.is_down && transition.device == RawInputDevice::Keyboard &&
                transition.code == SDL_SCANCODE_W)
                saw_key_release = true;
            if (!transition.is_down && transition.device == RawInputDevice::Mouse &&
                transition.code == SDL_BUTTON_RIGHT)
                saw_mouse_release = true;
        }
        ok = expect(saw_key_release && saw_mouse_release, "reset records release transitions for held raw sources") && ok;
        ok = expect(!input.is_key_released(SDL_SCANCODE_W) && !input.is_mouse_button_released(SDL_BUTTON_RIGHT),
            "reset retains legacy polling semantics without released edges") && ok;

        return ok;
    }

    bool testActionResetRelease()
    {
        InputManager input;
        InputActionSystem actions;
        actions.registerAction({ "Use", ActionValueType::Button, false });

        MappingContext gameplay;
        gameplay.name = "Gameplay";
        gameplay.bindings = {
            { "Use", InputBinding::Source::Key, SDL_SCANCODE_E },
            { "Use", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT }
        };
        bool ok = expect(actions.pushContext(gameplay), "reset release context activates");

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_E));
        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        actions.evaluate(input);
        ok = expect(actions.get("Use").held && actions.get("Use").press_count == 1,
            "combined key and mouse sources press one action") && ok;

        input.update();
        input.reset_state();
        ok = expect(!input.is_key_pressed(SDL_SCANCODE_E) && !input.is_key_released(SDL_SCANCODE_E) &&
            !input.is_mouse_button_pressed(SDL_BUTTON_LEFT) && !input.is_mouse_button_released(SDL_BUTTON_LEFT),
            "reset keeps raw InputManager polling reset-to-zero") && ok;
        actions.evaluate(input);
        ok = expect(actions.get("Use").released && !actions.get("Use").held &&
            actions.get("Use").release_count == 1,
            "raw state reset produces one action release after all sources clear") && ok;
        return ok;
    }

    bool testInputActionEdgesAndCounts()
    {
        InputManager input;
        InputActionSystem actions;
        actions.registerAction({ "Jump", ActionValueType::Button, false });

        MappingContext gameplay;
        gameplay.name = "Gameplay";
        gameplay.bindings = {
            { "Jump", InputBinding::Source::Key, SDL_SCANCODE_SPACE },
            { "Jump", InputBinding::Source::Key, SDL_SCANCODE_UP }
        };
        bool ok = true;
        ok = expect(actions.pushContext(gameplay), "valid action context registers and activates") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        actions.evaluate(input);
        ok = expect(actions.get("Jump").pressed && actions.get("Jump").held, "action key down reports pressed and held") && ok;
        ok = expect(actions.get("Jump").press_count == 1, "action key down increments press count") && ok;

        input.update();
        actions.evaluate(input);
        ok = expect(!actions.get("Jump").pressed && actions.get("Jump").held, "held action stays held without repeating pressed") && ok;

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_UP));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
        actions.evaluate(input);
        ok = expect(actions.get("Jump").held && !actions.get("Jump").released, "second binding keeps action held as first binding releases") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_UP));
        actions.evaluate(input);
        ok = expect(actions.get("Jump").released && !actions.get("Jump").held, "action releases after its final source releases") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
        actions.evaluate(input);
        ok = expect(actions.get("Jump").press_count == 2 && actions.get("Jump").release_count == 2,
            "multiple taps in one evaluation preserve both edge counts") && ok;
        ok = expect(actions.get("Jump").pressed && actions.get("Jump").released && !actions.get("Jump").held,
            "multiple taps expose both edge flags and final released state") && ok;
        ok = expect(!actions.wasPressed("Unregistered") && actions.axis("Unregistered") == glm::vec2(0.0f),
            "unknown action queries return released zero state") && ok;
        return ok;
    }

    bool testActionAxesAndMouseDelta()
    {
        InputManager input;
        InputActionSystem actions;
        actions.registerAction({ "Move", ActionValueType::Axis2D, false });
        actions.registerAction({ "Look", ActionValueType::Axis2D, false });
        actions.registerAction({ "Throttle", ActionValueType::Axis1D, false });
        actions.registerAction({ "Click", ActionValueType::Button, false });

        MappingContext gameplay;
        gameplay.name = "Gameplay";
        gameplay.bindings = {
            { "Move", InputBinding::Source::Key, SDL_SCANCODE_A, 1.0f, false, 0 },
            { "Move", InputBinding::Source::Key, SDL_SCANCODE_D, 1.0f, false, 0 },
            { "Move", InputBinding::Source::Key, SDL_SCANCODE_W, 1.0f, false, 1 },
            { "Move", InputBinding::Source::Key, SDL_SCANCODE_S, 1.0f, false, 1 },
            { "Throttle", InputBinding::Source::Key, SDL_SCANCODE_W, 1.0f },
            { "Throttle", InputBinding::Source::Key, SDL_SCANCODE_S, -1.0f },
            { "Look", InputBinding::Source::MouseDeltaX, 0, 1.0f, false, 0 },
            { "Look", InputBinding::Source::MouseDeltaY, 0, 1.0f, false, 1 },
            { "Click", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT }
        };
        bool ok = expect(actions.pushContext(gameplay), "axis context registers") ;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
        input.process_event(makeMouseMotionEvent(3.0f, 4.0f));
        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        actions.evaluate(input);

        const glm::vec2 move = actions.axis("Move");
        const glm::vec2 look = actions.axis("Look");
        ok = expect(actions.get("Move").held && std::abs(move.x - 0.7071067f) < 0.001f &&
            std::abs(move.y - 0.7071067f) < 0.001f, "Axis2D digital sources compose and clamp diagonal magnitude") && ok;
        ok = expect(actions.get("Look").held && std::abs(look.x - 0.6f) < 0.001f &&
            std::abs(look.y - 0.8f) < 0.001f, "mouse delta sources compose and clamp to unit length") && ok;
        ok = expect(actions.get("Look").press_count == 0 && actions.get("Look").release_count == 0,
            "mouse delta axes do not synthesize button edge counts") && ok;
        ok = expect(std::abs(actions.axis("Throttle").x - 1.0f) < 0.001f,
            "Axis1D keyboard sources produce a scaled signed value") && ok;
        ok = expect(actions.get("Click").pressed && actions.get("Click").held,
            "mouse button source activates a Button action") && ok;
        return ok;
    }

    bool testGamepadInputFeed()
    {
        constexpr Uint32 sdl_flags = SDL_INIT_EVENTS | SDL_INIT_GAMEPAD;
        const Uint32 already_initialized = SDL_WasInit(sdl_flags);
        const Uint32 to_initialize = sdl_flags & ~already_initialized;
        if (to_initialize != 0 && !SDL_InitSubSystem(to_initialize))
            return expect(false, "SDL gamepad subsystem initializes for virtual-device tests");

        SDL_JoystickID first_id = 0;
        SDL_JoystickID second_id = 0;
        SDL_JoystickID replacement_id = 0;
        bool ok = [&]() {
            InputManager input;
            input.clear_all_mappings();

            const int first_slot = firstFreeGamepadSlot(input);
            first_id = attachVirtualGamepad("Garden input test pad one");
            bool result = expect(first_id != 0 && first_slot >= 0,
                "virtual gamepad attaches into the first available logical slot");
            if (first_id == 0 || first_slot < 0)
                return result;
            pumpInputEvents(input);
            result = expect(input.gamepad(first_slot).connected,
                "gamepad-added event opens the virtual device") && result;

            InputActionSystem actions;
            actions.registerAction({ "PadTap", ActionValueType::Button, false });
            actions.registerAction({ "LeftStickX", ActionValueType::Axis1D, false });
            actions.registerAction({ "TriggerButton", ActionValueType::Button, false });
            actions.registerAction({ "MoveForward", ActionValueType::Button, false });
            actions.registerAction({ "MoveBack", ActionValueType::Button, false });
            actions.registerAction({ "PadLook", ActionValueType::Axis2D, false });
            MappingContext context;
            context.name = "GamepadTest";
            InputBinding button_binding;
            button_binding.action = "PadTap";
            button_binding.source = InputBinding::Source::GamepadButton;
            button_binding.code = SDL_GAMEPAD_BUTTON_SOUTH;
            button_binding.slot = first_slot;
            context.bindings.push_back(button_binding);
            InputBinding stick_binding;
            stick_binding.action = "LeftStickX";
            stick_binding.source = InputBinding::Source::GamepadAxis;
            stick_binding.code = SDL_GAMEPAD_AXIS_LEFTX;
            stick_binding.slot = first_slot;
            context.bindings.push_back(stick_binding);
            InputBinding trigger_binding;
            trigger_binding.action = "TriggerButton";
            trigger_binding.source = InputBinding::Source::GamepadAxis;
            trigger_binding.code = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
            trigger_binding.slot = first_slot;
            context.bindings.push_back(trigger_binding);
            InputBinding forward_binding;
            forward_binding.action = "MoveForward";
            forward_binding.source = InputBinding::Source::GamepadAxis;
            forward_binding.code = SDL_GAMEPAD_AXIS_LEFTY;
            forward_binding.invert = true;
            forward_binding.slot = first_slot;
            context.bindings.push_back(forward_binding);
            InputBinding back_binding = forward_binding;
            back_binding.action = "MoveBack";
            back_binding.invert = false;
            context.bindings.push_back(back_binding);
            InputBinding look_x_binding;
            look_x_binding.action = "PadLook";
            look_x_binding.source = InputBinding::Source::GamepadAxis;
            look_x_binding.code = SDL_GAMEPAD_AXIS_RIGHTX;
            look_x_binding.slot = first_slot;
            context.bindings.push_back(look_x_binding);
            InputBinding look_y_binding = look_x_binding;
            look_y_binding.code = SDL_GAMEPAD_AXIS_RIGHTY;
            look_y_binding.axis_target = 1;
            context.bindings.push_back(look_y_binding);
            result = expect(actions.pushContext(context), "gamepad bindings register with a logical slot") && result;

            entt::registry intent_registry;
            const entt::entity look_pawn = intent_registry.create();
            LocalPlayerInputActionNames look_actions;
            look_actions.look = "PadLook";
            auto& look_intent = intent_registry.emplace<LocalPlayerInputComponent>(look_pawn);
            look_intent.actions = look_actions;

            InputActionSystem restored_profile;
            restored_profile.registerAction({ "LeftStickX", ActionValueType::Axis1D, false });
            MappingContext restored_context;
            restored_context.name = "GamepadTest";
            restored_context.bindings.push_back(stick_binding);
            restored_profile.registerContext(restored_context);
            actions.rebind("GamepadTest", "LeftStickX", stick_binding);
            const std::string profile = actions.serialiseProfile();
            const std::string saved_slot = "\"slot\": " + std::to_string(first_slot);
            std::size_t profile_warnings = 0;
            result = expect(profile.find(saved_slot) != std::string::npos &&
                restored_profile.loadProfile(profile, &profile_warnings) && profile_warnings == 0 &&
                restored_profile.pushContext("GamepadTest"),
                "gamepad profile stores its logical slot and reloads without warnings") && result;

            SDL_Gamepad* gamepad = SDL_GetGamepadFromID(first_id);
            SDL_Joystick* joystick = gamepad ? SDL_GetGamepadJoystick(gamepad) : nullptr;
            result = expect(joystick != nullptr, "virtual gamepad is open for test input") && result;
            if (joystick)
            {
                input.update();
                SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
                pumpInputEvents(input);
                SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
                pumpInputEvents(input);
                actions.evaluate(input);
                const ActionState& tap = actions.get("PadTap");
                const uint32_t south_mask = uint32_t{ 1 } << SDL_GAMEPAD_BUTTON_SOUTH;
                result = expect(tap.pressed && tap.released && tap.press_count == 1 && tap.release_count == 1 && !tap.held,
                    "sub-frame gamepad tap preserves one press and release edge") && result;
                result = expect((input.gamepad(first_slot).buttons_pressed_mask & south_mask) != 0,
                    "gamepad pressed mask latches a tap for the current frame") && result;
                input.update();
                result = expect((input.gamepad(first_slot).buttons_pressed_mask & south_mask) == 0,
                    "gamepad pressed mask clears on the next input frame") && result;

                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 1638);
                pumpInputEvents(input);
                result = expect(glm::length(input.gamepad(first_slot).left_stick) == 0.0f,
                    "stick value within the radial dead zone reads zero") && result;
                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
                pumpInputEvents(input);
                actions.evaluate(input);
                restored_profile.evaluate(input);
                result = expect(std::abs(input.gamepad(first_slot).left_stick.x - 1.0f) < 0.001f &&
                    std::abs(actions.axis("LeftStickX").x - 1.0f) < 0.001f &&
                    std::abs(restored_profile.axis("LeftStickX").x - 1.0f) < 0.001f,
                    "full stick deflection rescales to full range and feeds an axis action") && result;

                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
                pumpInputEvents(input);
                actions.evaluate(input);
                updateLocalPlayerInputs(intent_registry, actions);
                result = expect(std::abs(look_intent.look.x - 1.0f) < 0.001f &&
                    std::abs(look_intent.look.y) < 0.001f,
                    "right-stick X action projects into local-player ECS look intent") && result;
                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, -32767);
                pumpInputEvents(input);
                actions.evaluate(input);
                updateLocalPlayerInputs(intent_registry, actions);
                result = expect(std::abs(glm::length(look_intent.look) - 1.0f) < 0.001f &&
                    look_intent.look.x > 0.7f && look_intent.look.y < -0.7f,
                    "right-stick diagonal remains normalized in the projected Look action") && result;

                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, -32767);
                pumpInputEvents(input);
                actions.evaluate(input);
                result = expect(actions.get("MoveForward").held && !actions.get("MoveBack").held,
                    "negative stick direction activates only its matching button action") && result;
                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, 32767);
                pumpInputEvents(input);
                actions.evaluate(input);
                result = expect(!actions.get("MoveForward").held && actions.get("MoveBack").held,
                    "positive stick direction activates only its matching button action") && result;

                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN + 1310);
                pumpInputEvents(input);
                actions.evaluate(input);
                result = expect(input.gamepad(first_slot).left_trigger == 0.0f && !actions.get("TriggerButton").held,
                    "trigger dead zone suppresses low input") && result;
                SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 19660);
                pumpInputEvents(input);
                actions.evaluate(input);
                result = expect(input.gamepad(first_slot).left_trigger > 0.5f && actions.get("TriggerButton").held,
                    "trigger axis crosses its button threshold after dead-zone rescaling") && result;

                SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
                pumpInputEvents(input);
                input.set_gamepad_input_enabled(false);
                actions.evaluate(input);
                result = expect(input.gamepad(first_slot).connected && input.gamepad(first_slot).buttons_held == 0 &&
                    input.gamepad(first_slot).left_trigger == 0.0f && !actions.get("TriggerButton").held &&
                    (input.gamepad(first_slot).buttons_pressed_mask & south_mask) != 0 &&
                    (input.gamepad(first_slot).buttons_released_mask & south_mask) != 0,
                    "gameplay focus loss clears pad intent without disconnecting the device") && result;
                input.update();
                input.set_gamepad_input_enabled(true);
                actions.evaluate(input);
                result = expect(actions.get("TriggerButton").held && !actions.get("TriggerButton").pressed,
                    "focus regain resamples a held trigger without a phantom press") && result;

                const int second_slot = firstFreeGamepadSlot(input);
                second_id = attachVirtualGamepad("Garden input test pad two");
                result = expect(second_id != 0 && second_slot >= 0 && second_slot != first_slot,
                    "second connected pad is assigned to a different free slot") && result;
                if (second_id != 0 && second_slot >= 0)
                {
                    pumpInputEvents(input);
                    result = expect(input.gamepad(second_slot).connected,
                        "second gamepad-added event fills the next free slot") && result;
                }

                SDL_DetachVirtualJoystick(first_id);
                first_id = 0;
                pumpInputEvents(input);
                actions.evaluate(input);
                result = expect(!input.gamepad(first_slot).connected &&
                    input.gamepad(first_slot).buttons_held == 0 &&
                    (input.gamepad(first_slot).buttons_released_mask & south_mask) != 0 &&
                    glm::length(input.gamepad(first_slot).left_stick) == 0.0f &&
                    input.gamepad(first_slot).left_trigger == 0.0f &&
                    (second_slot < 0 || input.gamepad(second_slot).connected) &&
                    !actions.get("PadTap").held && !actions.get("TriggerButton").held &&
                    actions.get("TriggerButton").released,
                    "unplug releases held actions and leaves other slots unchanged") && result;

                replacement_id = attachVirtualGamepad("Garden input test replacement pad");
                result = expect(replacement_id != 0, "replacement virtual gamepad attaches") && result;
                if (replacement_id != 0)
                {
                    pumpInputEvents(input);
                    result = expect(input.gamepad(first_slot).connected &&
                        (second_slot < 0 || input.gamepad(second_slot).connected),
                        "replacement uses the freed slot without compacting existing devices") && result;
                }
            }

            return result;
        }();

        if (first_id != 0)
            SDL_DetachVirtualJoystick(first_id);
        if (second_id != 0)
            SDL_DetachVirtualJoystick(second_id);
        if (replacement_id != 0)
            SDL_DetachVirtualJoystick(replacement_id);

        if (to_initialize != 0)
            SDL_QuitSubSystem(to_initialize);
        return ok;
    }

    bool testActionContextPriorityAndModal()
    {
        InputManager input;
        InputActionSystem actions;
        actions.registerAction({ "Top", ActionValueType::Button, true });
        actions.registerAction({ "Shadowed", ActionValueType::Button, false });
        actions.registerAction({ "Unrelated", ActionValueType::Button, false });
        actions.registerAction({ "Menu", ActionValueType::Button, false });

        MappingContext gameplay;
        gameplay.name = "Gameplay";
        gameplay.priority = 0;
        gameplay.bindings = {
            { "Shadowed", InputBinding::Source::Key, SDL_SCANCODE_F },
            { "Unrelated", InputBinding::Source::Key, SDL_SCANCODE_G }
        };
        MappingContext overlay;
        overlay.name = "Overlay";
        overlay.priority = 10;
        overlay.bindings = { { "Top", InputBinding::Source::Key, SDL_SCANCODE_F } };
        MappingContext menu;
        menu.name = "Menu";
        menu.priority = 20;
        menu.modal = true;
        menu.bindings = { { "Menu", InputBinding::Source::Key, SDL_SCANCODE_H } };

        bool ok = true;
        ok = expect(actions.registerContext(gameplay) && actions.registerContext(overlay) && actions.registerContext(menu),
            "priority contexts register") && ok;
        ok = expect(actions.pushContext("Gameplay") && actions.pushContext("Overlay"), "priority contexts activate") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_G));
        actions.evaluate(input);
        ok = expect(actions.get("Top").held && !actions.get("Shadowed").held,
            "consuming action hides only its source from lower contexts") && ok;
        ok = expect(actions.get("Unrelated").held, "consumption preserves unrelated lower-context sources") && ok;

        ok = expect(actions.pushContext("Menu"), "modal context activates") && ok;
        input.update();
        actions.evaluate(input);
        ok = expect(!actions.get("Unrelated").held && !actions.get("Shadowed").held,
            "modal context blocks lower contexts") && ok;
        actions.popContext("Menu");
        input.update();
        actions.evaluate(input);
        ok = expect(actions.get("Unrelated").held && !actions.get("Unrelated").pressed,
            "popping modal context restores held input without a phantom press") && ok;

        MappingContext reserved;
        reserved.name = "Reserved";
        reserved.bindings = { { "Menu", InputBinding::Source::Key, SDL_SCANCODE_ESCAPE } };
        ok = expect(!actions.registerContext(reserved), "reserved Escape source cannot be mapped") && ok;
        return ok;
    }

    bool testActionRebindCallbacksAndProfile()
    {
        InputActionSystem actions;
        actions.registerAction({ "Jump", ActionValueType::Button, false });
        actions.registerAction({ "Fire", ActionValueType::Button, false });

        MappingContext defaults;
        defaults.name = "Gameplay";
        defaults.priority = 3;
        defaults.bindings = {
            { "Jump", InputBinding::Source::Key, SDL_SCANCODE_SPACE },
            { "Jump", InputBinding::Source::Key, SDL_SCANCODE_UP },
            { "Fire", InputBinding::Source::Key, SDL_SCANCODE_F },
            { "Fire", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT }
        };
        bool ok = expect(actions.registerContext(defaults), "default context registers") ;

        InputBinding replacement;
        replacement.source = InputBinding::Source::Key;
        replacement.code = SDL_SCANCODE_F;
        ok = expect(actions.rebind("Gameplay", "Jump", replacement), "rebind accepts a valid source") && ok;
        InputBinding reserved;
        reserved.source = InputBinding::Source::Key;
        reserved.code = SDL_SCANCODE_GRAVE;
        ok = expect(!actions.rebind("Gameplay", "Jump", reserved), "rebind rejects reserved console key") && ok;

        const std::string profile = actions.serialiseProfile();
        nlohmann::json serialized = nlohmann::json::parse(profile);
        const auto& serialized_bindings = serialized["contexts"][0]["bindings"];
        ok = expect(serialized_bindings.size() == 1 && serialized_bindings[0]["action"] == "Jump" &&
            serialized_bindings[0]["code"] == SDL_SCANCODE_F,
            "profile stores only the explicit source override") && ok;

        InputActionSystem restored;
        restored.registerAction({ "Jump", ActionValueType::Button, false });
        restored.registerAction({ "Fire", ActionValueType::Button, false });
        ok = expect(restored.registerContext(defaults), "fresh system registers default context") && ok;
        std::size_t warnings = 0;
        ok = expect(restored.loadProfile(profile, &warnings) && warnings == 0, "saved action profile loads without warnings") && ok;
        MappingContext updated_defaults = defaults;
        updated_defaults.bindings.push_back({ "Jump", InputBinding::Source::Key, SDL_SCANCODE_LCTRL });
        ok = expect(restored.registerContext(updated_defaults), "updated defaults register over the loaded profile") && ok;

        nlohmann::json with_unknown = nlohmann::json::parse(profile);
        auto& bindings = with_unknown["contexts"][0]["bindings"];
        bindings.push_back({ { "action", "MissingAction" }, { "source", "key" }, { "code", SDL_SCANCODE_X } });
        bindings.push_back({ { "action", "Jump" }, { "source", "future_unknown_source" }, { "code", 0 } });
        InputActionSystem tolerant;
        tolerant.registerAction({ "Jump", ActionValueType::Button, false });
        tolerant.registerAction({ "Fire", ActionValueType::Button, false });
        tolerant.registerContext(defaults);
        ok = expect(tolerant.loadProfile(with_unknown.dump(), &warnings) && warnings == 2,
            "unknown action and unsupported source are skipped and counted") && ok;

        InputManager input;
        restored.pushContext("Gameplay");
        uint32_t callback_count = 0;
        const uint64_t callback_id = restored.bindCallback("Jump", [&callback_count](const ActionState&) {
            ++callback_count;
        });
        ok = expect(callback_id != 0, "callback bind returns an id") && ok;
        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F));
        restored.evaluate(input);
        ok = expect(restored.get("Jump").held && restored.get("Jump").press_count == 1,
            "rebound key activates Jump while preserving other Jump bindings") && ok;
        ok = expect(!restored.get("Fire").held, "same-source rebind clears conflicting action mapping") && ok;
        ok = expect(callback_count == 1, "action callback fires for the bound action change") && ok;

        ok = expect(restored.unbindCallback(callback_id), "callback unbind succeeds by id") && ok;
        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_F));
        restored.evaluate(input);
        ok = expect(callback_count == 1, "unbound callback is not invoked") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        restored.evaluate(input);
        ok = expect(restored.get("Jump").held, "rebind and profile loading retain other target action sources") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL));
        restored.evaluate(input);
        ok = expect(restored.get("Jump").held, "new default bindings survive profile application") && ok;

        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL));
        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        restored.evaluate(input);
        ok = expect(restored.get("Fire").held, "unrelated default sources survive source overrides") && ok;
        return ok;
    }

    bool testNetworkInputSampler()
    {
        InputManager input;
        InputActionSystem actions;
        for (const char* name : { "MoveForward", "MoveBack", "MoveLeft", "MoveRight", "Jump", "Use",
                 "Sprint", "Crouch", "Walk", "Drop", "Fire", "Reload", "WeaponSlot1", "WeaponSlot2",
                 "WeaponSlot3", "WeaponSlot4", "WeaponSlot5" })
            actions.registerAction({ name, ActionValueType::Button, false });

        MappingContext gameplay;
        gameplay.name = "Gameplay";
        gameplay.bindings = {
            { "MoveForward", InputBinding::Source::Key, SDL_SCANCODE_W },
            { "MoveBack", InputBinding::Source::Key, SDL_SCANCODE_S },
            { "MoveLeft", InputBinding::Source::Key, SDL_SCANCODE_A },
            { "MoveRight", InputBinding::Source::Key, SDL_SCANCODE_D },
            { "Jump", InputBinding::Source::Key, SDL_SCANCODE_SPACE },
            { "Use", InputBinding::Source::Key, SDL_SCANCODE_E },
            { "Sprint", InputBinding::Source::Key, SDL_SCANCODE_LSHIFT },
            { "Crouch", InputBinding::Source::Key, SDL_SCANCODE_LCTRL },
            { "Walk", InputBinding::Source::Key, SDL_SCANCODE_LALT },
            { "Drop", InputBinding::Source::Key, SDL_SCANCODE_G },
            { "Fire", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT },
            { "Reload", InputBinding::Source::Key, SDL_SCANCODE_R },
            { "WeaponSlot1", InputBinding::Source::Key, SDL_SCANCODE_1 },
            { "WeaponSlot2", InputBinding::Source::Key, SDL_SCANCODE_2 },
            { "WeaponSlot3", InputBinding::Source::Key, SDL_SCANCODE_3 },
            { "WeaponSlot4", InputBinding::Source::Key, SDL_SCANCODE_4 },
            { "WeaponSlot5", InputBinding::Source::Key, SDL_SCANCODE_5 }
        };

        bool ok = expect(actions.pushContext(gameplay), "FPSShooter sampler test context activates");
        input.update();
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_E));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LALT));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_G));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_R));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_2));
        input.process_event(makeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        actions.evaluate(input);

        const Net::InputState sampled = FPSShooter::sampleNetworkInput(actions, 1.25f, -0.5f, true, false, true, true);
        ok = expect(sampled.camera_yaw == 1.25f && sampled.camera_pitch == -0.5f,
            "sampler preserves camera angles") && ok;
        ok = expect((sampled.buttons & (Net::InputFlags::MOVE_FORWARD | Net::InputFlags::MOVE_BACK |
                Net::InputFlags::MOVE_LEFT | Net::InputFlags::MOVE_RIGHT)) ==
                (Net::InputFlags::MOVE_FORWARD | Net::InputFlags::MOVE_BACK | Net::InputFlags::MOVE_LEFT |
                    Net::InputFlags::MOVE_RIGHT) && sampled.move_forward == 0.0f && sampled.move_right == 0.0f,
            "sampler preserves simultaneous opposing movement flags") && ok;
        ok = expect((sampled.buttons & (Net::InputFlags::JUMP | Net::InputFlags::USE | Net::InputFlags::SPRINT |
                Net::InputFlags::CROUCH | Net::InputFlags::WALK | Net::InputFlags::DROP |
                Net::InputFlags::ATTACK | Net::InputFlags::RELOAD)) ==
                (Net::InputFlags::JUMP | Net::InputFlags::USE | Net::InputFlags::SPRINT |
                    Net::InputFlags::CROUCH | Net::InputFlags::WALK | Net::InputFlags::DROP |
                    Net::InputFlags::ATTACK | Net::InputFlags::RELOAD),
            "sampler maps held and edge actions to existing network flags") && ok;
        ok = expect(Net::getWeaponSelect(sampled.buttons) == 2, "sampler maps weapon selection to the network value field") && ok;

        const Net::InputState modal = FPSShooter::sampleNetworkInput(actions, 1.25f, -0.5f, true, true, true, true);
        ok = expect(modal.buttons == 0 && modal.move_forward == 0.0f && modal.move_right == 0.0f &&
            modal.camera_yaw == 1.25f && modal.camera_pitch == -0.5f,
            "sampler keeps camera values and blocks gameplay input while UI modal") && ok;
        const Net::InputState dead = FPSShooter::sampleNetworkInput(actions, 1.25f, -0.5f, false, false, true, true);
        ok = expect(dead.buttons == 0 && dead.move_forward == 0.0f && dead.move_right == 0.0f,
            "sampler blocks gameplay input while the local player is dead") && ok;
        const Net::InputState blocked_weapon = FPSShooter::sampleNetworkInput(actions, 1.25f, -0.5f, true, false, false, false);
        ok = expect((blocked_weapon.buttons & (Net::InputFlags::ATTACK | Net::InputFlags::RELOAD)) == 0,
            "sampler respects local fire and reload eligibility") && ok;
        return ok;
    }

    bool testInputRingBufferTickWrap()
    {
        Net::InputRingBuffer<4> history;
        const uint32_t ticks[] = { 0xFFFFFFFEu, 0xFFFFFFFFu, 0u };
        for (uint32_t tick : ticks)
        {
            Net::PredictionEntry entry;
            entry.tick = tick;
            entry.valid = true;
            history.push(entry);
        }

        std::vector<uint32_t> replay_ticks;
        history.forEachFrom(0xFFFFFFFFu, [&](const Net::PredictionEntry& entry) {
            replay_ticks.push_back(entry.tick);
        });
        bool ok = expect(replay_ticks == std::vector<uint32_t>{ 0u },
            "prediction history replays tick zero after tick 0xFFFFFFFF");

        history.discardUpTo(0xFFFFFFFFu);
        ok = expect(history.size() == 1 && history.get(0u) != nullptr &&
            history.get(0xFFFFFFFFu) == nullptr,
            "acknowledging tick 0xFFFFFFFF preserves wrapped tick zero") && ok;

        replay_ticks.clear();
        history.forEachFrom(0xFFFFFFFFu, [&](const Net::PredictionEntry& entry) {
            replay_ticks.push_back(entry.tick);
        });
        ok = expect(replay_ticks == std::vector<uint32_t>{ 0u },
            "wrapped tick zero remains replayable after acknowledgement") && ok;
        return ok;
    }

    bool testPredictionReplayCarriesAuthoritativeMovementState()
    {
        Net::InputRingBuffer<4> history;
        const uint32_t ticks[] = { 0xFFFFFFFFu, 1u };
        for (uint32_t tick : ticks)
        {
            Net::PredictionEntry entry;
            entry.tick = tick;
            entry.valid = true;
            entry.predicted_state.grounded = true;
            entry.predicted_state.ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
            history.push(entry);
        }

        Net::MovementState authoritative_state;
        authoritative_state.velocity = glm::vec3(10.0f, 0.0f, 0.0f);
        authoritative_state.grounded = false;
        authoritative_state.ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
        Net::MovementConfig movement_config;

        const auto apply_movement = [&](const Net::MovementInput& input, const Net::MovementState& start_state) {
            return Net::SharedMovement::simulate(input, start_state, movement_config);
        };

        uint32_t replayed_tick_count = 0;
        const Net::MovementState replayed_state = history.replayFrom(
            0xFFFFFFFEu, authoritative_state,
            [&](const Net::MovementInput& input, const Net::MovementState& start_state) {
                ++replayed_tick_count;
                return apply_movement(input, start_state);
            });

        Net::MovementState direct_airborne_replay = authoritative_state;
        history.forEachFrom(0xFFFFFFFEu, [&](const Net::PredictionEntry& entry) {
            direct_airborne_replay = apply_movement(entry.input, direct_airborne_replay);
        });

        Net::MovementState stale_grounded_replay = authoritative_state;
        history.forEachFrom(0xFFFFFFFEu, [&](const Net::PredictionEntry& entry) {
            stale_grounded_replay.grounded = entry.predicted_state.grounded;
            stale_grounded_replay.ground_normal = entry.predicted_state.ground_normal;
            stale_grounded_replay = apply_movement(entry.input, stale_grounded_replay);
        });

        const bool matches_direct_airborne =
            replayed_state.position.x == direct_airborne_replay.position.x &&
            replayed_state.position.y == direct_airborne_replay.position.y &&
            replayed_state.position.z == direct_airborne_replay.position.z &&
            replayed_state.velocity.x == direct_airborne_replay.velocity.x &&
            replayed_state.velocity.y == direct_airborne_replay.velocity.y &&
            replayed_state.velocity.z == direct_airborne_replay.velocity.z &&
            replayed_state.grounded == direct_airborne_replay.grounded &&
            replayed_state.ground_normal.x == direct_airborne_replay.ground_normal.x &&
            replayed_state.ground_normal.y == direct_airborne_replay.ground_normal.y &&
            replayed_state.ground_normal.z == direct_airborne_replay.ground_normal.z;
        const bool differs_from_stale_grounded =
            replayed_state.position.x != stale_grounded_replay.position.x ||
            replayed_state.position.y != stale_grounded_replay.position.y ||
            replayed_state.position.z != stale_grounded_replay.position.z ||
            replayed_state.velocity.x != stale_grounded_replay.velocity.x ||
            replayed_state.velocity.y != stale_grounded_replay.velocity.y ||
            replayed_state.velocity.z != stale_grounded_replay.velocity.z ||
            replayed_state.grounded != stale_grounded_replay.grounded ||
            replayed_state.ground_normal.x != stale_grounded_replay.ground_normal.x ||
            replayed_state.ground_normal.y != stale_grounded_replay.ground_normal.y ||
            replayed_state.ground_normal.z != stale_grounded_replay.ground_normal.z;

        bool ok = expect(matches_direct_airborne,
            "prediction replay carries authoritative airborne state across wrapped post-ack ticks");
        ok = expect(differs_from_stale_grounded,
            "SharedMovement replay differs from forcing stale grounded snapshots") && ok;
        ok = expect(std::abs(replayed_state.velocity.x - authoritative_state.velocity.x) < 0.0001f,
            "airborne SharedMovement preserves horizontal speed with neutral input") && ok;
        ok = expect(std::abs(replayed_state.velocity.x - stale_grounded_replay.velocity.x) > 0.1f,
            "stale grounded replay applies horizontal friction") && ok;
        ok = expect(history.get(0xFFFFFFFFu) != nullptr && history.get(1u) != nullptr,
            "prediction replay includes post-ack inputs across uint32 tick wrap") && ok;
        ok = expect(replayed_tick_count == 2,
            "prediction replay applies both newer inputs across uint32 tick wrap") && ok;
        return ok;
    }

    bool testReconciliationSmoothingFrameRateIndependence()
    {
        constexpr float elapsed_seconds = 0.5f;
        constexpr int frame_rates[] = { 30, 60, 120 };
        constexpr size_t frame_rate_count = sizeof(frame_rates) / sizeof(frame_rates[0]);
        float final_offsets[frame_rate_count]{};

        for (size_t rate_index = 0; rate_index < frame_rate_count; ++rate_index)
        {
            const int frame_rate = frame_rates[rate_index];
            Net::ReconciliationSmoothing smoothing;
            smoothing.onCorrection(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f));

            const int frame_count = static_cast<int>(elapsed_seconds * frame_rate);
            const float delta_time = 1.0f / static_cast<float>(frame_rate);
            for (int frame = 0; frame < frame_count; ++frame)
                smoothing.update(delta_time);

            final_offsets[rate_index] = glm::length(smoothing.visual_offset);
        }

        const float expected_60_hz_offset = 10.0f * std::pow(Net::ReconciliationSmoothing::DECAY_RATE, 30.0f);
        bool ok = true;
        for (size_t rate_index = 0; rate_index < frame_rate_count; ++rate_index)
        {
            ok = expect(final_offsets[rate_index] > Net::ReconciliationSmoothing::MIN_OFFSET,
                "reconciliation smoothing remains above its minimum offset after 0.5 seconds") && ok;
            ok = expect(std::abs(final_offsets[rate_index] - expected_60_hz_offset) < 0.0001f,
                "30, 60, and 120 Hz smoothing match the expected 60 Hz decay curve") && ok;
        }

        Net::ReconciliationSmoothing invalid_delta_smoothing;
        invalid_delta_smoothing.onCorrection(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f));
        invalid_delta_smoothing.update(0.0f);
        invalid_delta_smoothing.update(-1.0f);
        invalid_delta_smoothing.update(std::numeric_limits<float>::quiet_NaN());
        invalid_delta_smoothing.update(std::numeric_limits<float>::infinity());
        ok = expect(invalid_delta_smoothing.visual_offset.x == 10.0f &&
            invalid_delta_smoothing.visual_offset.y == 0.0f &&
            invalid_delta_smoothing.visual_offset.z == 0.0f,
            "invalid reconciliation delta times leave the current visual offset unchanged") && ok;
        return ok;
    }

    bool testLocalPlayerInputEcsProjection()
    {
        InputManager input;
        input.clear_all_mappings();
        input.update();

        InputActionSystem actions;
        const LocalPlayerInputActionNames names{
            "test.planar",
            "test.hop",
            "test.fast",
            "test.low",
            "test.fly",
            "test.boost",
            "test.look"
        };
        bool ok = true;
        ok = expect(actions.registerAction({ names.move, ActionValueType::Axis2D, false }),
            "local input test registers configured movement action") && ok;
        ok = expect(actions.registerAction({ names.jump, ActionValueType::Button, false }),
            "local input test registers configured jump action") && ok;
        ok = expect(actions.registerAction({ names.sprint, ActionValueType::Button, false }),
            "local input test registers configured sprint action") && ok;
        ok = expect(actions.registerAction({ names.crouch, ActionValueType::Button, false }),
            "local input test registers configured crouch action") && ok;
        ok = expect(actions.registerAction({ names.freecam_vertical, ActionValueType::Axis1D, false }),
            "local input test registers configured freecam axis") && ok;
        ok = expect(actions.registerAction({ names.freecam_fast, ActionValueType::Button, false }),
            "local input test registers configured freecam speed action") && ok;
        ok = expect(actions.registerAction({ names.look, ActionValueType::Axis2D, false }),
            "local input test registers configured look action") && ok;

        MappingContext context;
        context.name = "test.offline";
        context.bindings = {
            { names.move, InputBinding::Source::Key, SDL_SCANCODE_W, 1.0f, false, 1 },
            { names.move, InputBinding::Source::Key, SDL_SCANCODE_D, 1.0f, false, 0 },
            { names.jump, InputBinding::Source::Key, SDL_SCANCODE_SPACE },
            { names.sprint, InputBinding::Source::Key, SDL_SCANCODE_LSHIFT },
            { names.crouch, InputBinding::Source::Key, SDL_SCANCODE_LCTRL },
            { names.freecam_vertical, InputBinding::Source::Key, SDL_SCANCODE_SPACE },
            { names.freecam_vertical, InputBinding::Source::Key, SDL_SCANCODE_LSHIFT, 1.0f, true },
            { names.freecam_fast, InputBinding::Source::Key, SDL_SCANCODE_LSHIFT },
            { names.look, InputBinding::Source::Key, SDL_SCANCODE_RIGHT, 1.0f, false, 0 },
            { names.look, InputBinding::Source::Key, SDL_SCANCODE_DOWN, 1.0f, false, 1 }
        };
        ok = expect(actions.pushContext(context), "configured local input context activates") && ok;

        entt::registry registry;
        const entt::entity pawn = registry.create();
        auto& local = registry.emplace<LocalPlayerInputComponent>(pawn);
        local.actions = names;

        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_W));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RIGHT));
        input.process_event(makeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_DOWN));
        actions.evaluate(input);
        updateLocalPlayerInputs(registry, actions);

        const float diagonal_axis = 1.0f / std::sqrt(2.0f);
        ok = expect(std::abs(local.move.x - diagonal_axis) < 0.001f &&
            std::abs(local.move.y - diagonal_axis) < 0.001f,
            "ECS input system copies configured planar action values") && ok;
        ok = expect(local.jump_held && local.sprint_held && local.crouch_held,
            "ECS input system preserves held player action semantics") && ok;
        ok = expect(std::abs(local.look.x - diagonal_axis) < 0.001f &&
            std::abs(local.look.y - diagonal_axis) < 0.001f,
            "ECS input system copies configured look action values") && ok;
        ok = expect(std::abs(local.freecam_move.x + diagonal_axis) < 0.001f &&
            std::abs(local.freecam_move.y) < 0.001f &&
            std::abs(local.freecam_move.z - diagonal_axis) < 0.001f && local.freecam_fast,
            "ECS input system translates configured freecam intent") && ok;

        input.update();
        actions.evaluate(input);
        updateLocalPlayerInputs(registry, actions);
        ok = expect(local.jump_held,
            "offline jump remains held across evaluations instead of becoming an edge") && ok;

        local.input_enabled = false;
        updateLocalPlayerInputs(registry, actions);
        ok = expect(std::abs(local.move.x) < 0.001f && std::abs(local.move.y) < 0.001f && !local.jump_held &&
            !local.sprint_held && !local.crouch_held &&
            glm::length(local.look) < 0.001f &&
            glm::length(local.freecam_move) < 0.001f && !local.freecam_fast,
            "disabled local input clears every derived intent") && ok;

        entt::registry ownership_registry;
        const entt::entity first_pawn = ownership_registry.create();
        const entt::entity second_pawn = ownership_registry.create();
        entt::entity current_owner = entt::null;
        setLocalPlayerInputOwner(ownership_registry, current_owner, first_pawn, names);
        ok = expect(current_owner == first_pawn &&
            ownership_registry.all_of<LocalPlayerInputComponent>(first_pawn),
            "local input owner receives the transient component") && ok;
        setLocalPlayerInputOwner(ownership_registry, current_owner, second_pawn, names);
        ok = expect(current_owner == second_pawn &&
            !ownership_registry.all_of<LocalPlayerInputComponent>(first_pawn) &&
            ownership_registry.all_of<LocalPlayerInputComponent>(second_pawn),
            "reassigned local ownership removes input from the previous pawn") && ok;
        setLocalPlayerInputOwner(ownership_registry, current_owner, entt::null, names);
        ok = expect(current_owner == entt::null &&
            !ownership_registry.all_of<LocalPlayerInputComponent>(second_pawn),
            "cleared local ownership leaves no remote pawn input component") && ok;
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
    ok = testActionResetRelease() && ok;
    ok = testInputActionEdgesAndCounts() && ok;
    ok = testActionAxesAndMouseDelta() && ok;
    ok = testGamepadInputFeed() && ok;
    ok = testActionContextPriorityAndModal() && ok;
    ok = testActionRebindCallbacksAndProfile() && ok;
    ok = testNetworkInputSampler() && ok;
    ok = testInputRingBufferTickWrap() && ok;
    ok = testPredictionReplayCarriesAuthoritativeMovementState() && ok;
    ok = testReconciliationSmoothingFrameRateIndependence() && ok;
    ok = testLocalPlayerInputEcsProjection() && ok;

    if (!ok)
        return 1;

    std::printf("InputTests passed\n");
    return 0;
}
