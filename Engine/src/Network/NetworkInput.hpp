#pragma once

#include "NetworkProtocol.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Net {

constexpr uint32_t MAX_INPUT_TICK_WINDOW = 128;
constexpr uint32_t MAX_INPUT_BURST_TICKS = 8;
constexpr uint8_t INPUT_ACTION_LATCH_MASK = InputFlags::JUMP | InputFlags::ATTACK | InputFlags::ATTACK2;

inline bool isTickNewer(uint32_t tick, uint32_t last_tick)
{
    return tick != last_tick && static_cast<int32_t>(tick - last_tick) > 0;
}

inline InputSample makePrimaryInputSample(const InputCommandMessage& msg)
{
    InputSample sample;
    sample.tick = msg.client_tick;
    sample.buttons = msg.buttons;
    sample.camera_yaw = msg.camera_yaw;
    sample.camera_pitch = msg.camera_pitch;
    sample.move_forward = msg.move_forward;
    sample.move_right = msg.move_right;
    return sample;
}

inline std::vector<InputSample> collectInputSamplesChronological(
    const InputCommandMessage& msg,
    const std::vector<InputSample>& redundant_inputs)
{
    std::vector<InputSample> samples;
    samples.reserve(redundant_inputs.size() + 1);
    for (const auto& input : redundant_inputs) {
        samples.push_back(input);
    }
    samples.push_back(makePrimaryInputSample(msg));

    std::sort(samples.begin(), samples.end(), [](const InputSample& a, const InputSample& b) {
        return static_cast<int32_t>(a.tick - b.tick) < 0;
    });

    return samples;
}

inline bool shouldAcceptInputTick(uint32_t tick, uint32_t last_processed_tick, uint32_t max_window = MAX_INPUT_TICK_WINDOW)
{
    if (tick == 0 || !isTickNewer(tick, last_processed_tick)) {
        return false;
    }

    if (last_processed_tick != 0 && (tick - last_processed_tick) > max_window) {
        return false;
    }

    return true;
}

inline void accrueInputTickBudget(
    uint32_t current_server_tick,
    uint32_t& last_budget_server_tick,
    uint32_t& available_ticks,
    uint32_t max_burst = MAX_INPUT_BURST_TICKS)
{
    if (last_budget_server_tick == 0) {
        last_budget_server_tick = current_server_tick;
        available_ticks = (std::min)(available_ticks, max_burst);
        return;
    }

    if (!isTickNewer(current_server_tick, last_budget_server_tick)) {
        return;
    }

    const uint32_t elapsed_ticks = current_server_tick - last_budget_server_tick;
    available_ticks = (std::min)(available_ticks + elapsed_ticks, max_burst);
    last_budget_server_tick = current_server_tick;
}

inline bool consumeInputTickBudget(uint32_t& available_ticks)
{
    if (available_ticks == 0) {
        return false;
    }

    --available_ticks;
    return true;
}

} // namespace Net
