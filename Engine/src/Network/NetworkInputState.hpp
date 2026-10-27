#pragma once

#include <cstdint>

namespace Net {

// Client input sampled for a single network command.
struct InputState
{
    uint16_t buttons = 0;             // Button flags (uint16, wire v3)
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;        // -1.0 to 1.0
    float move_right = 0.0f;          // -1.0 to 1.0
};

}
