#pragma once

#include "SharedComponents.hpp"
#include "WeaponTypes.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstdlib>
#include <cmath>

// Shared weapon logic used by both client (prediction) and server (authoritative).
namespace WeaponSystem
{
    struct AccuracyContext
    {
        float horizontal_speed = 0.0f;
        float max_speed = 10.0f;
        bool grounded = true;
    };

    inline float decayTowardZero(float value, float delta_time, float recovery_time)
    {
        if (value <= 0.0f)
            return 0.0f;
        if (delta_time <= 0.0f || recovery_time <= 0.0f)
            return value;

        const float factor = std::exp(-delta_time * std::log(10.0f) / recovery_time);
        value *= factor;
        return value < 0.0001f ? 0.0f : value;
    }

    inline float computeSpread(const WeaponComponent& wc, const AccuracyContext& context)
    {
        const auto& def = getWeaponDef(wc.weapon_type);
        const float move_start = std::max(context.max_speed * 0.34f, 0.001f);
        const float move_end = std::max(context.max_speed * 0.95f, move_start + 0.001f);
        const float move_alpha = std::clamp(
            (context.horizontal_speed - move_start) / (move_end - move_start), 0.0f, 1.0f);
        const float move_curve = std::pow(move_alpha, 0.25f);

        float spread = def.spread;
        spread += std::clamp(wc.accuracy_penalty, 0.0f, def.max_spread);
        spread += move_curve * def.move_spread;
        if (!context.grounded)
            spread += def.air_spread;
        if (wc.reloading)
            spread += def.reload_spread;

        return std::clamp(spread, def.spread, def.max_spread);
    }

    // Update weapon cooldowns and reload timers. Call once per tick.
    inline void tick(WeaponComponent& wc, float delta_time)
    {
        delta_time = std::max(delta_time, 0.0f);
        const auto& def = getWeaponDef(wc.weapon_type);

        if (wc.fire_cooldown > 0.0f)
            wc.fire_cooldown = std::max(wc.fire_cooldown - delta_time, 0.0f);

        wc.time_since_last_shot += delta_time;
        wc.accuracy_penalty = decayTowardZero(wc.accuracy_penalty, delta_time, def.recovery_time);
        if (wc.time_since_last_shot > def.fire_rate * def.recoil_decay_delay)
            wc.recoil_index = decayTowardZero(wc.recoil_index, delta_time, 1.0f / std::max(def.recoil_decay_rate, 0.001f));

        if (wc.reloading)
        {
            wc.reload_timer -= delta_time;
            if (wc.reload_timer <= 0.0f)
            {
                const int32_t needed = std::max(wc.max_ammo - wc.ammo, 0);
                const int32_t loaded = std::min(needed, wc.reserve_ammo);
                wc.ammo += loaded;
                wc.reserve_ammo -= loaded;
                wc.reloading = false;
                wc.reload_timer = 0.0f;
            }
        }
    }

    // Attempt to fire. Returns true if shot was fired (consumes ammo, sets cooldown).
    inline bool tryFire(WeaponComponent& wc)
    {
        if (!wc.canFire()) return false;

        const auto& def = getWeaponDef(wc.weapon_type);
        wc.ammo--;
        wc.fire_cooldown = def.fire_rate;
        wc.accuracy_penalty = std::min(wc.accuracy_penalty + def.shot_spread_penalty, def.max_spread);
        wc.recoil_index += 1.0f;
        wc.time_since_last_shot = 0.0f;

        // Auto-reload on empty
        if (wc.ammo <= 0 && wc.reserve_ammo > 0 && !wc.reloading)
        {
            wc.reloading = true;
            wc.reload_timer = def.reload_time;
        }

        return true;
    }

    // Start manual reload. Returns true if reload started.
    inline bool tryReload(WeaponComponent& wc)
    {
        const auto& def = getWeaponDef(wc.weapon_type);
        wc.max_ammo = def.max_ammo;
        wc.max_reserve_ammo = def.max_reserve_ammo;
        if (!wc.canReload()) return false;

        wc.reloading = true;
        wc.reload_timer = def.reload_time;
        return true;
    }

    // Generate a spread-adjusted ray direction for a single pellet.
    // Uses a simple uniform random spread within a cone.
    // seed should differ per pellet (e.g. pellet_index * 7919 + tick * 31).
    inline glm::vec3 applySpread(const glm::vec3& direction, float spread_angle, uint32_t seed)
    {
        if (spread_angle <= 0.0f) return direction;

        // Simple pseudo-random from seed
        auto hash = [](uint32_t x) -> float {
            x ^= x >> 16;
            x *= 0x45d9f3bU;
            x ^= x >> 16;
            x *= 0x45d9f3bU;
            x ^= x >> 16;
            return static_cast<float>(x & 0xFFFF) / 65535.0f;
        };

        float angle = std::sqrt(hash(seed)) * spread_angle;
        float rot = hash(seed + 1) * 6.28318f; // Random rotation around forward axis

        // Create perpendicular axes
        glm::vec3 up = (std::abs(direction.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(direction, up));
        glm::vec3 actual_up = glm::cross(right, direction);

        // Offset direction within cone
        float sin_a = std::sin(angle);
        glm::vec3 offset = right * (sin_a * std::cos(rot)) + actual_up * (sin_a * std::sin(rot));
        return glm::normalize(direction * std::cos(angle) + offset);
    }
} // namespace WeaponSystem
