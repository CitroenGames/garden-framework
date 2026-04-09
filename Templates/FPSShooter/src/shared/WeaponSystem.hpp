#pragma once

#include "SharedComponents.hpp"
#include "WeaponTypes.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdlib>
#include <cmath>

// Shared weapon logic used by both client (prediction) and server (authoritative).
namespace WeaponSystem
{
    // Update weapon cooldowns and reload timers. Call once per tick.
    inline void tick(WeaponComponent& wc, float delta_time)
    {
        if (wc.fire_cooldown > 0.0f)
            wc.fire_cooldown -= delta_time;

        if (wc.reloading)
        {
            wc.reload_timer -= delta_time;
            if (wc.reload_timer <= 0.0f)
            {
                const auto& def = getWeaponDef(wc.weapon_type);
                wc.ammo = def.max_ammo;
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

        // Auto-reload on empty
        if (wc.ammo <= 0 && !wc.reloading)
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
        if (wc.reloading || wc.ammo >= def.max_ammo) return false;

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

        float angle = hash(seed) * spread_angle;
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
