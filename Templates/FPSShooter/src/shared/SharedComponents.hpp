#pragma once

#include <cstdint>
#include <string>

// Health component for damageable entities
struct HealthComponent
{
    int32_t health = 100;
    int32_t max_health = 100;
    bool alive = true;
    float respawn_timer = 0.0f;     // Server-side countdown to respawn (seconds)
    float invulnerable_timer = 0.0f; // Brief invulnerability after respawn

    HealthComponent() = default;
    HealthComponent(int32_t hp) : health(hp), max_health(hp) {}

    void takeDamage(int32_t damage)
    {
        if (!alive || invulnerable_timer > 0.0f) return;
        health -= damage;
        if (health <= 0) {
            health = 0;
            alive = false;
        }
    }

    void reset()
    {
        health = max_health;
        alive = true;
        respawn_timer = 0.0f;
        invulnerable_timer = 1.5f; // 1.5s invulnerability on respawn
    }
};

// Weapon type enumeration
enum class WeaponType : uint8_t
{
    RIFLE = 0,
    SHOTGUN = 1,
    COUNT
};

// Weapon component attached to player entities
struct WeaponComponent
{
    WeaponType weapon_type = WeaponType::RIFLE;
    int32_t ammo = 30;
    int32_t max_ammo = 30;
    int32_t reserve_ammo = 90;
    int32_t max_reserve_ammo = 90;
    float fire_cooldown = 0.0f;     // Time until can fire again (seconds)
    float reload_timer = 0.0f;      // Time remaining in reload (seconds)
    float accuracy_penalty = 0.0f;  // Additional spread accumulated by recent shots
    float recoil_index = 0.0f;      // Burst length used by HUD/recoil-aware effects
    float time_since_last_shot = 1000.0f;
    bool reloading = false;

    WeaponComponent() = default;
    WeaponComponent(WeaponType type) : weapon_type(type) {}

    bool canFire() const { return ammo > 0 && fire_cooldown <= 0.0f && !reloading; }
    bool canReload() const { return !reloading && ammo < max_ammo && reserve_ammo > 0; }
};

// Score tracking per player
struct ScoreComponent
{
    int32_t kills = 0;
    int32_t deaths = 0;

    ScoreComponent() = default;
};
