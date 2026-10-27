#pragma once

#include "NetworkInput.hpp"
#include "SharedMovement.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace Net {

// Tracks visual smoothing offset for reconciliation.
// When the server corrects our position, instead of snapping we blend the visual
// offset to zero over ~100ms for a smooth correction.
struct ReconciliationSmoothing
{
    glm::vec3 visual_offset = glm::vec3(0.0f);  // Current visual offset from logical position
    static constexpr float DECAY_RATE = 0.85f;   // Per-frame decay (~100ms at 60fps to reach near-zero)
    static constexpr float MIN_OFFSET = 0.001f;   // Below this, snap to zero

    // Call when reconciliation produces a corrected position.
    // old_display_pos: where we were visually before correction
    // new_logical_pos: where correction says we should be
    void onCorrection(const glm::vec3& old_display_pos, const glm::vec3& new_logical_pos)
    {
        visual_offset = old_display_pos - new_logical_pos;
    }

    // Decay at a frame-rate-independent rate while preserving the 60 Hz curve.
    void update(float delta_time)
    {
        if (!std::isfinite(delta_time) || delta_time <= 0.0f)
            return;

        visual_offset *= std::pow(DECAY_RATE, delta_time * 60.0f);
        if (glm::length(visual_offset) < MIN_OFFSET)
            visual_offset = glm::vec3(0.0f);
    }

    // Get the smoothed display position
    glm::vec3 getDisplayPosition(const glm::vec3& logical_position) const
    {
        return logical_position + visual_offset;
    }
};

// One entry in the client's prediction history.
// Stores the input that was sent and the predicted state that resulted.
struct PredictionEntry
{
    uint32_t tick = 0;
    MovementInput input;
    MovementState predicted_state; // State AFTER applying this input
    bool valid = false;
};

// Fixed-size ring buffer of prediction entries for client-side prediction.
// Stores the last N inputs so they can be replayed during server reconciliation.
template <size_t N = 128>
class InputRingBuffer
{
public:
    void push(const PredictionEntry& entry)
    {
        entries[head] = entry;
        head = (head + 1) % N;
        if (count < N)
            count++;
    }

    // Discard all entries with tick <= given tick (they've been acknowledged by server)
    void discardUpTo(uint32_t tick)
    {
        // We don't physically remove - just mark invalid.
        // Iteration skips invalid entries and entries at or before the given tick.
        for (size_t i = 0; i < count; i++)
        {
            size_t idx = (head + N - count + i) % N;
            if (entries[idx].valid && !isTickNewer(entries[idx].tick, tick))
            {
                entries[idx].valid = false;
            }
        }

        // Compact: advance past leading invalid entries
        while (count > 0)
        {
            size_t oldest = (head + N - count) % N;
            if (!entries[oldest].valid)
                count--;
            else
                break;
        }
    }

    // Get entry at a specific tick (returns nullptr if not found)
    const PredictionEntry* get(uint32_t tick) const
    {
        for (size_t i = 0; i < count; i++)
        {
            size_t idx = (head + N - count + i) % N;
            if (entries[idx].valid && entries[idx].tick == tick)
                return &entries[idx];
        }
        return nullptr;
    }

    // Iterate all valid entries newer than from_tick, in chronological order.
    // Callback signature: void(const PredictionEntry&)
    template <typename Func>
    void forEachFrom(uint32_t from_tick, Func&& callback) const
    {
        for (size_t i = 0; i < count; i++)
        {
            size_t idx = (head + N - count + i) % N;
            if (entries[idx].valid && isTickNewer(entries[idx].tick, from_tick))
            {
                callback(entries[idx]);
            }
        }
    }

    // Replay unacknowledged inputs in tick order, carrying each movement result
    // into the next step instead of restoring stale predicted state snapshots.
    template <typename Func>
    MovementState replayFrom(uint32_t from_tick, MovementState state, Func&& apply_movement) const
    {
        forEachFrom(from_tick, [&](const PredictionEntry& entry) {
            state = apply_movement(entry.input, state);
        });
        return state;
    }

    size_t size() const { return count; }

    void clear()
    {
        head = 0;
        count = 0;
        for (auto& e : entries)
            e.valid = false;
    }

private:
    std::array<PredictionEntry, N> entries{};
    size_t head = 0;
    size_t count = 0;
};

} // namespace Net
