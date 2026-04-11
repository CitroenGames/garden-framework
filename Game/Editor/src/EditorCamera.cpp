#include "EditorCamera.hpp"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

EditorCamera::EditorCamera()
{
    cam = camera(0.0f, 5.0f, -10.0f);
}

void EditorCamera::update(float dt, bool looking_active,
                           float mouse_dx, float mouse_dy,
                           const bool* keyboard_state)
{
    if (looking_active)
    {
        cam.rotation.y -= mouse_dx * sensitivity;
        cam.rotation.x += mouse_dy * sensitivity;
        // Clamp pitch to avoid gimbal flip
        cam.rotation.x = std::max(-1.5f, std::min(1.5f, cam.rotation.x));
    }

    // WASD + QE fly movement — only when looking (right mouse held)
    if (looking_active)
    {
        glm::vec3 local(0.0f);
        if (keyboard_state[SDL_SCANCODE_W]) local.z += 1.0f;
        if (keyboard_state[SDL_SCANCODE_S]) local.z -= 1.0f;
        if (keyboard_state[SDL_SCANCODE_A]) local.x += 1.0f;
        if (keyboard_state[SDL_SCANCODE_D]) local.x -= 1.0f;
        if (keyboard_state[SDL_SCANCODE_E]) local.y += 1.0f;
        if (keyboard_state[SDL_SCANCODE_Q]) local.y -= 1.0f;

        float len = glm::length(local);
        if (len > 0.001f)
        {
            local /= len;
            float speed = keyboard_state[SDL_SCANCODE_LSHIFT] ? movement_speed * 3.0f : movement_speed;
            cam.position += cam.camera_rot_quaternion() * local * speed * dt;
        }
    }
}

void EditorCamera::adjustSpeed(float scroll_delta)
{
    movement_speed *= std::pow(1.1f, scroll_delta);
    movement_speed = std::clamp(movement_speed, 0.1f, 500.0f);
}
