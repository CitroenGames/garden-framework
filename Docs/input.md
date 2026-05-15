# Input

Garden uses SDL3 for input. The host pumps SDL events each frame and exposes state through `InputManager`. Game modules read it from `g_services->input_manager`.

## Per-frame state

```cpp
#include "InputManager.hpp"
#include <SDL3/SDL.h>

void gardenGameUpdate(float dt)
{
    auto* input = g_services->input_manager;
    if (!input)
        return;

    // Edge-triggered: true only on the frame the key went down.
    if (input->is_key_pressed(SDL_SCANCODE_SPACE))
        jump();

    // Level-triggered: true while held.
    if (input->is_key_held(SDL_SCANCODE_W))
        moveForward(dt);

    // Edge-triggered release.
    if (input->is_key_released(SDL_SCANCODE_E))
        endInteract();

    // Mouse delta is raw pixels accumulated for this frame.
    const float mouse_dx = input->get_mouse_delta_x();
    const float mouse_dy = input->get_mouse_delta_y();
    yaw   -= mouse_dx * mouse_sens;
    pitch -= mouse_dy * mouse_sens;

    if (input->is_mouse_button_pressed(SDL_BUTTON_LEFT))
        shoot();
}
```

Use scancodes (`SDL_SCANCODE_*`), not keycodes. Scancodes are layout-independent, which keeps gameplay bindings stable across keyboard layouts.

## Action Mappings

Use action mappings for commands that should be rebound or shared by systems. Register mappings at startup, bind callbacks, and keep direct key polling for local movement axes where that is simpler.

```cpp
input->add_action_mapping("Interact", SDL_SCANCODE_E);
input->bind_action("Interact", [](InputActionState state) {
    if (state == InputActionState::Pressed)
        beginInteract();
});
```

`InputActionState::Pressed` and `Released` are dispatched from SDL transitions. Key repeat events are ignored.

## Mouse Capture

For FPS-style mouse look, lock the cursor with SDL relative mouse mode while gameplay owns input and disable it when opening menus or pausing. The standalone game host does this through `InputHandler`; the editor routes input separately during Play-In-Editor.

## Per-entity Input Components

`InputComponent` (in `Engine/src/Components/InputComponent.hpp`) is the base for attaching input-driven behavior to entities. For tightly coupled local-player code, reading `InputManager` directly in `gardenGameUpdate` is usually simpler.

## What Does Not Go Through InputManager

- Editor UI input is handled by ImGui. If UI has keyboard or mouse capture, gameplay input should be ignored for that event.
- HUD input is handled by RmlUi when it owns focus. Either gate gameplay input on UI focus or design HUD screens as read-only during play.
- Network input is serialized through `MovementInput` / `InputState` structs. Servers do not read SDL.

