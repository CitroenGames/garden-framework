#pragma once

#include "SharedMovement.hpp"
#include <array>
#include <cstdint>
#include <cstddef>

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
        // We don't physically remove — just mark invalid.
        // Iteration skips invalid entries and entries with tick <= given.
        for (size_t i = 0; i < count; i++)
        {
            size_t idx = (head + N - count + i) % N;
            if (entries[idx].valid && entries[idx].tick <= tick)
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

    // Iterate all valid entries with tick > from_tick, in chronological order.
    // Callback signature: void(const PredictionEntry&)
    template <typename Func>
    void forEachFrom(uint32_t from_tick, Func&& callback) const
    {
        for (size_t i = 0; i < count; i++)
        {
            size_t idx = (head + N - count + i) % N;
            if (entries[idx].valid && entries[idx].tick > from_tick)
            {
                callback(entries[idx]);
            }
        }
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
