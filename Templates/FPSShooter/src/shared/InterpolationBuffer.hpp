#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <cstddef>

// A single snapshot of a remote entity's state at a specific server tick
struct InterpolationSnapshot
{
    uint32_t tick = 0;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    bool grounded = false;
    bool valid = false;
};

// Per-entity buffer that stores recent snapshots and interpolates between them.
// Remote entities are rendered at (server_tick - interp_delay) to ensure
// we always have two snapshots to interpolate between.
class EntityInterpolationBuffer
{
public:
    static constexpr size_t MAX_SNAPSHOTS = 32;
    static constexpr float TELEPORT_THRESHOLD = 10.0f; // Snap instead of lerp if delta > this

    void addSnapshot(uint32_t tick, const glm::vec3& pos, const glm::vec3& vel, bool grounded)
    {
        // Insert in chronological order (most recent at head)
        buffer[head] = {tick, pos, vel, grounded, true};
        head = (head + 1) % MAX_SNAPSHOTS;
        if (count < MAX_SNAPSHOTS)
            count++;
    }

    // Interpolate to a fractional render tick.
    // Returns true if interpolation succeeded, false if insufficient data.
    bool interpolate(float render_tick, glm::vec3& out_position) const
    {
        if (count < 2) {
            // Not enough data — snap to latest if available
            if (count == 1) {
                size_t idx = (head + MAX_SNAPSHOTS - 1) % MAX_SNAPSHOTS;
                out_position = buffer[idx].position;
                return true;
            }
            return false;
        }

        // Find two surrounding snapshots: a.tick <= render_tick <= b.tick
        const InterpolationSnapshot* snap_a = nullptr;
        const InterpolationSnapshot* snap_b = nullptr;

        // Iterate from oldest to newest
        for (size_t i = 0; i < count; i++) {
            size_t idx = (head + MAX_SNAPSHOTS - count + i) % MAX_SNAPSHOTS;
            const auto& snap = buffer[idx];
            if (!snap.valid) continue;

            if (static_cast<float>(snap.tick) <= render_tick) {
                snap_a = &snap;
            } else {
                snap_b = &snap;
                break;
            }
        }

        if (snap_a && snap_b) {
            // Interpolate between the two snapshots
            float range = static_cast<float>(snap_b->tick - snap_a->tick);
            float t = (range > 0.0f) ? (render_tick - static_cast<float>(snap_a->tick)) / range : 0.0f;
            t = glm::clamp(t, 0.0f, 1.0f);

            // Check for teleport
            if (glm::distance(snap_a->position, snap_b->position) > TELEPORT_THRESHOLD) {
                out_position = snap_b->position; // Snap
            } else {
                out_position = glm::mix(snap_a->position, snap_b->position, t);
            }
            return true;
        }

        if (snap_a) {
            // Past the latest snapshot — use most recent position (slight extrapolation hold)
            out_position = snap_a->position;
            return true;
        }

        if (snap_b) {
            // Before the earliest snapshot — snap to earliest
            out_position = snap_b->position;
            return true;
        }

        return false;
    }

    void clear()
    {
        head = 0;
        count = 0;
        for (auto& s : buffer)
            s.valid = false;
    }

private:
    std::array<InterpolationSnapshot, MAX_SNAPSHOTS> buffer{};
    size_t head = 0;
    size_t count = 0;
};
