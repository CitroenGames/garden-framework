#pragma once

#include "Network/PredictionTypes.hpp"

#include <cstdint>

// Runtime-only client prediction state. This belongs to the locally owned
// pawn so respawn, despawn, and local-owner changes naturally retire the
// replay history and presentation correction with that pawn.
struct ClientPredictionComponent
{
    Net::InputRingBuffer<128> input_history;
    Net::ReconciliationSmoothing smoothing;

    uint32_t last_authoritative_tick = 0;
    bool has_authoritative_tick = false;

    uint64_t correction_count = 0;
    uint64_t replayed_input_count = 0;
    uint64_t stale_update_count = 0;
    uint64_t replay_failure_count = 0;
    float last_error_meters = 0.0f;
    float max_error_meters = 0.0f;
};
