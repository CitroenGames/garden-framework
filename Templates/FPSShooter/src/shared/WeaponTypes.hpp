#pragma once

#include "SharedComponents.hpp"
#include <cstdint>

// Weapon definition table -- data-driven weapon stats.
// Add new weapon types by extending the WeaponType enum and adding an entry here.
struct WeaponDef
{
    const char* name;
    int32_t damage;         // Damage per hit (per pellet for shotgun)
    float fire_rate;        // Seconds between shots
    float range;            // Max hitscan distance (meters)
    int32_t max_ammo;       // Magazine size
    int32_t max_reserve_ammo;
    float reload_time;      // Seconds to reload
    int pellets;            // Number of pellets per shot (>1 for shotgun)
    float spread;           // Standing base cone half-angle in radians
    float move_spread;      // Additional spread at full running speed
    float air_spread;       // Additional spread while airborne
    float reload_spread;    // Additional spread while reload is in progress
    float shot_spread_penalty;
    float max_spread;
    float recovery_time;
    float recoil_decay_delay;
    float recoil_decay_rate;
};

// Weapon definitions indexed by WeaponType
inline const WeaponDef& getWeaponDef(WeaponType type)
{
    static const WeaponDef defs[] = {
        // RIFLE: accurate first shot, wider during sustained fire and movement.
        { "Rifle",   15, 0.10f, 200.0f, 30, 90, 1.55f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        // SHOTGUN: close-range burst damage with broad pellet spread.
        { "Shotgun", 10, 0.80f,  40.0f,  8, 32, 2.00f, 8, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
    };

    static_assert(sizeof(defs) / sizeof(defs[0]) == static_cast<size_t>(WeaponType::COUNT),
                  "WeaponDef table must match WeaponType::COUNT");

    uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= static_cast<uint8_t>(WeaponType::COUNT))
        idx = 0;

    return defs[idx];
}

// Initialize a WeaponComponent from its type definition
inline void initWeapon(WeaponComponent& wc, WeaponType type)
{
    const auto& def = getWeaponDef(type);
    wc.weapon_type = type;
    wc.ammo = def.max_ammo;
    wc.max_ammo = def.max_ammo;
    wc.reserve_ammo = def.max_reserve_ammo;
    wc.max_reserve_ammo = def.max_reserve_ammo;
    wc.fire_cooldown = 0.0f;
    wc.reload_timer = 0.0f;
    wc.accuracy_penalty = 0.0f;
    wc.recoil_index = 0.0f;
    wc.time_since_last_shot = 1000.0f;
    wc.reloading = false;
}
