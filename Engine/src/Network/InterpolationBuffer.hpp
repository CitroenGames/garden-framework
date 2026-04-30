#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>

namespace Net {

// A single snapshot of a remote entity's state at a specific server tick
struct InterpolationSnapshot
{
    uint32_t tick = 0;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    float rotation_y = 0.0f;
    bool grounded = false;
    bool valid = false;
};

// Per-entity buffer that stores recent snapshots and interpolates between them.
// Remote entities are rendered at (server_tick - interp_delay) to ensure
// we always have two snapshots to interpolate between.
//
// Features:
//  - Cubic Hermite interpolation using velocity tangents for smooth curves
//  - Velocity-based extrapolation when packets are late (capped to MAX_EXTRAP_TICKS)
//  - Rotation interpolation with shortest-path angle wrapping
//  - Teleport detection (snaps instead of lerps for large deltas)
class EntityInterpolationBuffer
{
public:
    static constexpr size_t MAX_SNAPSHOTS = 32;
    static constexpr float TELEPORT_THRESHOLD = 10.0f; // Snap instead of lerp if delta > this
    static constexpr float MAX_EXTRAP_TICKS = 12.0f;   // Max extrapolation ~200ms at 60Hz
    static constexpr float TICK_DURATION = 1.0f / 60.0f; // Duration of one tick in seconds

    void addSnapshot(uint32_t tick, const glm::vec3& pos, const glm::vec3& vel,
                     bool grounded, float rot_y = 0.0f)
    {
        buffer[head] = {tick, pos, vel, rot_y, grounded, true};
        head = (head + 1) % MAX_SNAPSHOTS;
        if (count < MAX_SNAPSHOTS)
            count++;
    }

    // Interpolate to a fractional render tick.
    // Returns true if interpolation succeeded, false if insufficient data.
    bool interpolate(float render_tick, glm::vec3& out_position, float& out_rotation) const
    {
        if (count < 1) return false;

        if (count == 1) {
            size_t idx = (head + MAX_SNAPSHOTS - 1) % MAX_SNAPSHOTS;
            out_position = buffer[idx].position;
            out_rotation = buffer[idx].rotation_y;
            return true;
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
            // Hermite interpolation between snapshots
            float range = static_cast<float>(snap_b->tick - snap_a->tick);
            float t = (range > 0.0f) ? (render_tick - static_cast<float>(snap_a->tick)) / range : 0.0f;
            t = glm::clamp(t, 0.0f, 1.0f);

            // Check for teleport
            if (glm::distance(snap_a->position, snap_b->position) > TELEPORT_THRESHOLD) {
                out_position = snap_b->position;
                out_rotation = snap_b->rotation_y;
                return true;
            }

            // Cubic Hermite interpolation using velocity as tangents
            // h(t) = (2t^3 - 3t^2 + 1)*p0 + (t^3 - 2t^2 + t)*m0 + (-2t^3 + 3t^2)*p1 + (t^3 - t^2)*m1
            float dt = range * TICK_DURATION; // Time span in seconds between snapshots
            glm::vec3 m0 = snap_a->velocity * dt; // Tangent at start
            glm::vec3 m1 = snap_b->velocity * dt; // Tangent at end

            float t2 = t * t;
            float t3 = t2 * t;

            float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            float h10 = t3 - 2.0f * t2 + t;
            float h01 = -2.0f * t3 + 3.0f * t2;
            float h11 = t3 - t2;

            out_position = h00 * snap_a->position + h10 * m0 + h01 * snap_b->position + h11 * m1;

            // Rotation: shortest-path lerp
            out_rotation = lerpAngle(snap_a->rotation_y, snap_b->rotation_y, t);

            return true;
        }

        if (snap_a) {
            // Past the latest snapshot - extrapolate using velocity (capped)
            float extrap_ticks = render_tick - static_cast<float>(snap_a->tick);
            if (extrap_ticks > MAX_EXTRAP_TICKS) extrap_ticks = MAX_EXTRAP_TICKS;

            float extrap_time = extrap_ticks * TICK_DURATION;
            out_position = snap_a->position + snap_a->velocity * extrap_time;
            out_rotation = snap_a->rotation_y;
            return true;
        }

        if (snap_b) {
            // Before earliest snapshot - snap to earliest
            out_position = snap_b->position;
            out_rotation = snap_b->rotation_y;
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

    // Lerp between two angles (in degrees or radians) via shortest path
    static float lerpAngle(float a, float b, float t)
    {
        float diff = b - a;
        // Wrap to [-180, 180] for degrees (works for radians too with [-PI, PI])
        // Using a generous range since rotation_y is stored in degrees in TransformComponent
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        return a + diff * t;
    }
};

} // namespace Net
