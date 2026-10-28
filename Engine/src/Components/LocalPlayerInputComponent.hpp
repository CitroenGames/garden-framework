#pragma once

#include <glm/glm.hpp>

#include <string>

// Runtime-only input ownership and intent for one locally controlled pawn.
// Action names are supplied by the host/game profile; EngineCore does not
// assume a particular game's action vocabulary.
struct LocalPlayerInputActionNames
{
    std::string move;
    std::string jump;
    std::string sprint;
    std::string crouch;
    std::string freecam_vertical;
    std::string freecam_fast;
    std::string look;
};

struct LocalPlayerInputComponent
{
    LocalPlayerInputActionNames actions;
    bool input_enabled = true;

    // Intent refreshed from the host's evaluated actions before controller
    // update. These fields are deliberately transient and unreflected.
    glm::vec2 move{ 0.0f };
    bool jump_held = false;
    // Retain a short press until a character simulation step consumes it.
    bool jump_press_pending = false;
    bool sprint_held = false;
    bool crouch_held = false;
    glm::vec2 look{ 0.0f };
    glm::vec3 freecam_move{ 0.0f };
    bool freecam_fast = false;

    void clearIntent()
    {
        move = glm::vec2(0.0f);
        jump_held = false;
        jump_press_pending = false;
        sprint_held = false;
        crouch_held = false;
        look = glm::vec2(0.0f);
        freecam_move = glm::vec3(0.0f);
        freecam_fast = false;
    }
};
