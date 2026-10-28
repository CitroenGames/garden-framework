#pragma once

#include <cstdint>
#include <string>

// Template-owned world-item bounds shared by authoritative dropped-weapon
// lifecycle validation and its presentation protocol. EngineCore deliberately
// has no knowledge of this FPS-specific world rule.
inline constexpr float kWorldItemMinimumWorldY = -50.0f;

// Template health (distinct from engine ::HealthComponent in GameplayComponents.hpp;
// both headers may be included together, no macro aliasing).
struct FpsHealthComponent
{
    int32_t health = 100;
    int32_t max_health = 100;
    bool alive = true;
    float respawn_timer = 0.0f;     // Server-side countdown to respawn (seconds)
    float invulnerable_timer = 0.0f; // Brief invulnerability after respawn

    FpsHealthComponent() = default;
    FpsHealthComponent(int32_t hp) : health(hp), max_health(hp) {}

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

// Server-side latch for the template death lifecycle.  `resolved` prevents a
// second lethal input from re-running drops, scoring, or event publication
// before this pawn is respawned.  The sequence is local to the pawn and is
// carried into the bounded death event for stable attribution after teardown.
struct FpsDeathLifecycleComponent
{
    uint32_t sequence = 0;
    bool resolved = false;
};

// Weapon type enumeration. Values double as inventory item_def ids (+1,
// since InventoryComponent reserves item_def_id 0 for empty).
enum class WeaponType : uint8_t
{
    RIFLE = 0,
    SHOTGUN = 1,
    PISTOL = 2,
    KNIFE = 3,
    GRENADE = 4,
    // Catalog weapons (spec S4.2). Legacy ids above keep their behaviour.
    GLOCK = 5, P2000 = 6, USP_S = 7, P250 = 8, TEC9 = 9, FIVESEVEN = 10,
    CZ75 = 11, ELITE = 12, DEAGLE = 13, REVOLVER = 14,
    NOVA = 15, XM1014 = 16, SAWEDOFF = 17, MAG7 = 18, M249 = 19, NEGEV = 20,
    MAC10 = 21, MP9 = 22, MP7 = 23, MP5SD = 24, UMP45 = 25, P90 = 26, BIZON = 27,
    FAMAS = 28, GALILAR = 29, AK47 = 30, M4A4 = 31, M4A1S = 32, SG556 = 33,
    AUG = 34, SSG08 = 35, AWP = 36, G3SG1 = 37, SCAR20 = 38, TASER = 39,
    HEGRENADE = 40, FLASHBANG = 41, SMOKEGRENADE = 42, MOLOTOV = 43,
    INCENDIARY = 44, DECOY = 45,
    COUNT
};

// Game slot semantics over engine InventoryComponent slots (indices 0..4).
enum class WeaponSlot : uint8_t
{
    Primary = 0,
    Secondary = 1,
    Melee = 2,
    Grenade = 3,
    Utility = 4,
    COUNT
};

struct WeaponSlotState
{
    WeaponType type = WeaponType::RIFLE;
    int32_t ammo = 0;
    int32_t reserve = 0;
    bool occupied = false;
};

// Per-player loadout: ammo per slot. Presence mirrors InventoryComponent.
struct PlayerLoadoutComponent
{
    WeaponSlotState slots[static_cast<size_t>(WeaponSlot::COUNT)];
    int32_t active_slot = static_cast<int32_t>(WeaponSlot::Primary);
};

// World pickup created by DROP / death. Position lives in TransformComponent.
struct DroppedWeaponComponent
{
    WeaponType weapon_type = WeaponType::RIFLE;
    int32_t ammo = 0;
    int32_t reserve = 0;
    uint16_t dropped_by = 0;
    float despawn_timer = 30.0f;
};

// Stable template protocol identity for a server-authoritative dropped item.
// This intentionally differs from Net::NetworkedEntity: world-item messages
// identify a client presentation object and do not make the item an EngineCore
// snapshot entity.
struct WorldItemReplicationIdComponent
{
    uint32_t value = 0;
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

// Game-side armor/equipment state (buy system; spec S7 armor is game data).
struct ArmorComponent
{
    int32_t armor = 0;          // 0..100
    bool has_helmet = false;
    bool has_defuse_kit = false; // kit flag (defuse timing hook, next stage)
};

// Score tracking per player
struct ScoreComponent
{
    int32_t kills = 0;
    int32_t deaths = 0;

    ScoreComponent() = default;
};
