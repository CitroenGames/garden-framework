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
    float reload_time;      // Seconds to reload
    int pellets;            // Number of pellets per shot (>1 for shotgun)
    float spread;           // Cone half-angle in radians (0 = perfectly accurate)
};

// Weapon definitions indexed by WeaponType
inline const WeaponDef& getWeaponDef(WeaponType type)
{
    static const WeaponDef defs[] = {
        // RIFLE: accurate, moderate damage, fast fire rate
        { "Rifle",   15,   0.1f,   200.0f,  30,  1.5f,  1,  0.01f  },
        // SHOTGUN: high damage up close, spread, slow fire rate
        { "Shotgun", 12,   0.8f,   40.0f,   8,   2.0f,  8,  0.08f  },
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
    wc.fire_cooldown = 0.0f;
    wc.reload_timer = 0.0f;
    wc.reloading = false;
}
