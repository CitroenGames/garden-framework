#include "InputManager.hpp"
#include "InputActionSystem.hpp"
#include "NetworkInputSampler.hpp"
#include "json.hpp"

#include <SDL3/SDL.h>
#include <cmath>
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
        bindings.push_back({ { "action", "Jump" }, { "source", "gamepad_button" }, { "code", 0 } });
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
    ok = testActionContextPriorityAndModal() && ok;
    ok = testActionRebindCallbacksAndProfile() && ok;
    ok = testNetworkInputSampler() && ok;

    if (!ok)
        return 1;

    std::printf("InputTests passed\n");
    return 0;
}
