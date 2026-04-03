#pragma once

#include "Components/camera.hpp"
#include <SDL.h>
#include <glm/glm.hpp>

class EditorCamera
{
public:
    camera cam;
    float movement_speed = 8.0f;
    float fast_speed = 24.0f;
    float sensitivity = 0.002f;   // radians per pixel

    EditorCamera();

    // Call once per frame.
    // looking_active: true when right mouse button is held (relative mouse mode on).
    // mouse_dx/dy: accumulated pixel deltas for this frame.
    // keyboard_state: from SDL_GetKeyboardState(nullptr).
    void update(float dt, bool looking_active,
                float mouse_dx, float mouse_dy,
                const Uint8* keyboard_state);
};
