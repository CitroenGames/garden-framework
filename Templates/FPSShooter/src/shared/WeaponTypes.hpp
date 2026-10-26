#pragma once

#include "SharedComponents.hpp"
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Buy catalog (spec S4.2 roster + S5 grenades as defs-only + armor/kit).
// 44 entries, every entry purchasable (price > 0). Knife/C4 stay legacy
// WeaponTypes (price 0, not sold) and are not catalog entries.
// Data only: [P] durations/awards live in Competitive ModeConfig/EconomyConfig.
// ---------------------------------------------------------------------------
enum class BuyItemKind : uint8_t { Weapon = 0, Grenade = 1, Equipment = 2 };

// team_mask bits: 1 = Attackers (T), 2 = Defenders (CT), 3 = Both.
struct WeaponCatalogDef
{
    uint16_t id;
    const char* alias;
    const char* name;
    int32_t price;
    int32_t kill_award;
    int32_t damage;
    float cycle_time;       // seconds between shots
    int32_t mag;
    int32_t reserve;
    float range_modifier;
    float armor_ratio;
    int32_t pellets;
    int32_t move_speed;
    WeaponSlot slot;
    uint8_t team_mask;
    WeaponType weapon_type; // WeaponType::COUNT for equipment (no weapon)
    BuyItemKind kind;
};

inline constexpr uint8_t kCatalogTeamAttackers = 1;
inline constexpr uint8_t kCatalogTeamDefenders = 2;
inline constexpr uint8_t kCatalogTeamBoth = 3;

inline const WeaponCatalogDef* weaponCatalog(size_t& out_count)
{
    static const WeaponCatalogDef defs[] = {
        // id alias name price kill dmg cycle mag reserve rangemod armor pellets speed slot mask type kind
        {  1, "glock",     "Glock-18",       200, 300,  30, 0.15f,  20, 120, 0.85f, 0.94f, 1, 240, WeaponSlot::Secondary, kCatalogTeamAttackers, WeaponType::GLOCK, BuyItemKind::Weapon },
        {  2, "hkp2000",   "P2000",          200, 300,  35, 0.17f,  13,  52, 0.91f, 1.01f, 1, 240, WeaponSlot::Secondary, kCatalogTeamDefenders, WeaponType::P2000, BuyItemKind::Weapon },
        {  3, "usp_silencer", "USP-S",       200, 300,  35, 0.17f,  12,  24, 0.91f, 1.01f, 1, 240, WeaponSlot::Secondary, kCatalogTeamDefenders, WeaponType::USP_S, BuyItemKind::Weapon },
        {  4, "p250",      "P250",           300, 300,  38, 0.15f,  13,  26, 0.90f, 1.28f, 1, 240, WeaponSlot::Secondary, kCatalogTeamBoth, WeaponType::P250, BuyItemKind::Weapon },
        {  5, "tec9",      "Tec-9",          500, 300,  33, 0.12f,  18,  90, 0.79f, 1.81f, 1, 240, WeaponSlot::Secondary, kCatalogTeamAttackers, WeaponType::TEC9, BuyItemKind::Weapon },
        {  6, "fiveseven", "Five-SeveN",     500, 300,  32, 0.15f,  20, 100, 0.81f, 1.82f, 1, 240, WeaponSlot::Secondary, kCatalogTeamDefenders, WeaponType::FIVESEVEN, BuyItemKind::Weapon },
        {  7, "cz75a",     "CZ75-Auto",      500, 100,  31, 0.10f,  12,  12, 0.85f, 1.55f, 1, 240, WeaponSlot::Secondary, kCatalogTeamBoth, WeaponType::CZ75, BuyItemKind::Weapon },
        {  8, "elite",     "Dual Berettas",  300, 300,  38, 0.12f,  30, 120, 0.79f, 1.15f, 1, 240, WeaponSlot::Secondary, kCatalogTeamBoth, WeaponType::ELITE, BuyItemKind::Weapon },
        {  9, "deagle",    "Desert Eagle",   700, 300,  53, 0.225f,  7,  35, 0.85f, 1.86f, 1, 230, WeaponSlot::Secondary, kCatalogTeamBoth, WeaponType::DEAGLE, BuyItemKind::Weapon },
        { 10, "revolver",  "R8 Revolver",    600, 300,  86, 0.50f,   8,   8, 0.94f, 1.86f, 1, 180, WeaponSlot::Secondary, kCatalogTeamBoth, WeaponType::REVOLVER, BuyItemKind::Weapon },
        { 11, "nova",      "Nova",          1050, 900,  26, 0.88f,   8,  32, 0.70f, 1.00f, 9, 220, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::NOVA, BuyItemKind::Weapon },
        { 12, "xm1014",    "XM1014",        2000, 900,  20, 0.35f,   7,  32, 0.70f, 1.60f, 6, 215, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::XM1014, BuyItemKind::Weapon },
        { 13, "sawedoff",  "Sawed-Off",     1100, 900,  32, 0.85f,   7,  32, 0.45f, 1.50f, 8, 210, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::SAWEDOFF, BuyItemKind::Weapon },
        { 14, "mag7",      "MAG-7",         1300, 900,  30, 0.85f,   5,  32, 0.45f, 1.50f, 8, 225, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::MAG7, BuyItemKind::Weapon },
        { 15, "m249",      "M249",          5200, 300,  32, 0.08f, 100, 200, 0.97f, 1.60f, 1, 195, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::M249, BuyItemKind::Weapon },
        { 16, "negev",     "Negev",         1700, 300,  35, 0.075f,150, 300, 0.97f, 1.42f, 1, 150, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::NEGEV, BuyItemKind::Weapon },
        { 17, "mac10",     "MAC-10",        1050, 600,  29, 0.075f, 30, 100, 0.80f, 1.15f, 1, 240, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::MAC10, BuyItemKind::Weapon },
        { 18, "mp9",       "MP9",           1250, 600,  26, 0.07f,  30, 120, 0.87f, 1.20f, 1, 240, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::MP9, BuyItemKind::Weapon },
        { 19, "mp7",       "MP7",           1500, 600,  29, 0.08f,  30, 120, 0.85f, 1.25f, 1, 220, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::MP7, BuyItemKind::Weapon },
        { 20, "mp5sd",     "MP5-SD",        1500, 600,  27, 0.08f,  30, 120, 0.85f, 1.25f, 1, 235, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::MP5SD, BuyItemKind::Weapon },
        { 21, "ump45",     "UMP-45",        1200, 600,  35, 0.09f,  25, 100, 0.75f, 1.30f, 1, 230, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::UMP45, BuyItemKind::Weapon },
        { 22, "p90",       "P90",           2350, 300,  26, 0.07f,  50, 100, 0.86f, 1.38f, 1, 230, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::P90, BuyItemKind::Weapon },
        { 23, "bizon",     "PP-Bizon",      1400, 600,  27, 0.08f,  64, 120, 0.80f, 1.26f, 1, 240, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::BIZON, BuyItemKind::Weapon },
        { 24, "famas",     "FAMAS",         2050, 300,  30, 0.09f,  25,  90, 0.96f, 1.40f, 1, 220, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::FAMAS, BuyItemKind::Weapon },
        { 25, "galilar",   "Galil AR",      1800, 300,  30, 0.09f,  35,  90, 0.98f, 1.55f, 1, 215, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::GALILAR, BuyItemKind::Weapon },
        { 26, "ak47",      "AK-47",         2700, 300,  36, 0.10f,  30,  90, 0.98f, 1.55f, 1, 215, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::AK47, BuyItemKind::Weapon },
        { 27, "m4a1",      "M4A4",          3100, 300,  33, 0.09f,  30,  90, 0.97f, 1.40f, 1, 225, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::M4A4, BuyItemKind::Weapon },
        { 28, "m4a1_silencer", "M4A1-S",    2900, 300,  38, 0.10f,  20,  80, 0.94f, 1.40f, 1, 225, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::M4A1S, BuyItemKind::Weapon },
        { 29, "sg556",     "SG 553",        3000, 300,  30, 0.11f,  30,  90, 0.98f, 2.00f, 1, 210, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::SG556, BuyItemKind::Weapon },
        { 30, "aug",       "AUG",           3300, 300,  28, 0.10f,  30,  90, 0.98f, 1.80f, 1, 220, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::AUG, BuyItemKind::Weapon },
        { 31, "ssg08",     "SSG 08",        1700, 300,  88, 1.25f,  10,  90, 0.98f, 1.70f, 1, 230, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::SSG08, BuyItemKind::Weapon },
        { 32, "awp",       "AWP",           4750, 100, 115, 1.455f,  5,  30, 0.99f, 1.95f, 1, 200, WeaponSlot::Primary, kCatalogTeamBoth, WeaponType::AWP, BuyItemKind::Weapon },
        { 33, "g3sg1",     "G3SG1",         5000, 300,  80, 0.25f,  20,  90, 0.98f, 1.65f, 1, 215, WeaponSlot::Primary, kCatalogTeamAttackers, WeaponType::G3SG1, BuyItemKind::Weapon },
        { 34, "scar20",    "SCAR-20",       5000, 300,  80, 0.25f,  20,  90, 0.98f, 1.65f, 1, 215, WeaponSlot::Primary, kCatalogTeamDefenders, WeaponType::SCAR20, BuyItemKind::Weapon },
        { 35, "taser",     "Zeus x27",       200,   0, 500, 0.15f,   1,   0, 0.0049f, 2.00f, 1, 220, WeaponSlot::Utility, kCatalogTeamBoth, WeaponType::TASER, BuyItemKind::Weapon },
        // Grenades: defs only in v1 (throwing is a later stage).
        { 36, "hegrenade", "HE Grenade",     300, 300,  99, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamBoth, WeaponType::HEGRENADE, BuyItemKind::Grenade },
        { 37, "flashbang", "Flashbang",      200, 300,   0, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamBoth, WeaponType::FLASHBANG, BuyItemKind::Grenade },
        { 38, "smokegrenade", "Smoke Grenade", 300, 300, 0, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamBoth, WeaponType::SMOKEGRENADE, BuyItemKind::Grenade },
        { 39, "molotov",   "Molotov",        400, 300,   0, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamAttackers, WeaponType::MOLOTOV, BuyItemKind::Grenade },
        { 40, "incgrenade", "Incendiary",    600, 300,   0, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamDefenders, WeaponType::INCENDIARY, BuyItemKind::Grenade },
        { 41, "decoy",     "Decoy Grenade",   50, 300,   0, 0.80f,   1,   0, 0.99f, 1.00f, 1, 240, WeaponSlot::Grenade, kCatalogTeamBoth, WeaponType::DECOY, BuyItemKind::Grenade },
        // Equipment (armor/kit are purchasable items, not weapons).
        { 42, "vest",      "Kevlar Vest",    650,   0,   0, 0.0f,    0,   0, 0.0f, 0.0f, 0, 0, WeaponSlot::Utility, kCatalogTeamBoth, WeaponType::COUNT, BuyItemKind::Equipment },
        { 43, "vesthelm",  "Kevlar+Helmet", 1000,   0,   0, 0.0f,    0,   0, 0.0f, 0.0f, 0, 0, WeaponSlot::Utility, kCatalogTeamBoth, WeaponType::COUNT, BuyItemKind::Equipment },
        { 44, "defuser",   "Defuse Kit",     200,   0,   0, 0.0f,    0,   0, 0.0f, 0.0f, 0, 0, WeaponSlot::Utility, kCatalogTeamDefenders, WeaponType::COUNT, BuyItemKind::Equipment },
    };
    out_count = sizeof(defs) / sizeof(defs[0]);
    return defs;
}

inline const WeaponCatalogDef* findCatalogById(uint16_t id)
{
    size_t n = 0; const WeaponCatalogDef* defs = weaponCatalog(n);
    for (size_t i = 0; i < n; ++i) if (defs[i].id == id) return &defs[i];
    return nullptr;
}
inline const WeaponCatalogDef* findCatalogByAlias(const char* alias)
{
    if (!alias) return nullptr;
    size_t n = 0; const WeaponCatalogDef* defs = weaponCatalog(n);
    for (size_t i = 0; i < n; ++i) if (std::strcmp(defs[i].alias, alias) == 0) return &defs[i];
    return nullptr;
}
inline const WeaponCatalogDef* findCatalogByWeaponType(WeaponType type)
{
    size_t n = 0; const WeaponCatalogDef* defs = weaponCatalog(n);
    for (size_t i = 0; i < n; ++i) if (defs[i].weapon_type == type) return &defs[i];
    return nullptr;
}

// Weapon definition table -- data-driven weapon stats (behaviour).
struct WeaponDef
{
    const char* name;
    int32_t damage;
    float fire_rate;
    float range;
    int32_t max_ammo;
    int32_t max_reserve_ammo;
    float reload_time;
    int pellets;
    float spread;
    float move_spread;
    float air_spread;
    float reload_spread;
    float shot_spread_penalty;
    float max_spread;
    float recovery_time;
    float recoil_decay_delay;
    float recoil_decay_rate;
};

inline const WeaponDef& getWeaponDef(WeaponType type)
{
    // Legacy entries (ids 0..4) unchanged. Catalog entries mirror the
    // catalog damage/cycle/mag/reserve/pellets above; reload is class-flat
    // (pistols 2.2s, primaries 2.5s per spec S4.1 [P]).
    static const WeaponDef defs[] = {
        { "Rifle",   15, 0.10f, 200.0f, 30, 90, 1.55f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Shotgun", 10, 0.80f,  40.0f,  8, 32, 2.00f, 8, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
        { "Pistol",  12, 0.25f, 100.0f, 12, 48, 1.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Knife",   50, 0.40f,   2.0f, 999, 0, 0.10f, 1, 0.001f, 0.001f, 0.001f, 0.001f, 0.0010f, 0.010f, 0.10f, 1.10f, 6.0f },
        { "Grenade",  0, 0.80f,  20.0f,  1,  0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Glock-18", 30, 0.15f, 200.0f, 20, 120, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "P2000", 35, 0.17f, 200.0f, 13, 52, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "USP-S", 35, 0.17f, 200.0f, 12, 24, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "P250", 38, 0.15f, 200.0f, 13, 26, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Tec-9", 33, 0.12f, 200.0f, 18, 90, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Five-SeveN", 32, 0.15f, 200.0f, 20, 100, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "CZ75-Auto", 31, 0.10f, 200.0f, 12, 12, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Dual Berettas", 38, 0.12f, 200.0f, 30, 120, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Desert Eagle", 53, 0.225f, 200.0f, 7, 35, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "R8 Revolver", 86, 0.50f, 200.0f, 8, 8, 2.20f, 1, 0.012f, 0.030f, 0.050f, 0.025f, 0.0080f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Nova", 26, 0.88f, 40.0f, 8, 32, 2.50f, 9, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
        { "XM1014", 20, 0.35f, 40.0f, 7, 32, 2.50f, 6, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
        { "Sawed-Off", 32, 0.85f, 40.0f, 7, 32, 2.50f, 8, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
        { "MAG-7", 30, 0.85f, 40.0f, 5, 32, 2.50f, 8, 0.055f, 0.025f, 0.040f, 0.030f, 0.0150f, 0.120f, 0.50f, 1.10f, 5.0f },
        { "M249", 32, 0.08f, 200.0f, 100, 200, 2.50f, 1, 0.015f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "Negev", 35, 0.075f, 200.0f, 150, 300, 2.50f, 1, 0.015f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "MAC-10", 29, 0.075f, 200.0f, 30, 100, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "MP9", 26, 0.07f, 200.0f, 30, 120, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "MP7", 29, 0.08f, 200.0f, 30, 120, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "MP5-SD", 27, 0.08f, 200.0f, 30, 120, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "UMP-45", 35, 0.09f, 200.0f, 25, 100, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "P90", 26, 0.07f, 200.0f, 50, 100, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "PP-Bizon", 27, 0.08f, 200.0f, 64, 120, 2.50f, 1, 0.020f, 0.035f, 0.050f, 0.025f, 0.0100f, 0.090f, 0.30f, 1.10f, 6.0f },
        { "FAMAS", 30, 0.09f, 200.0f, 25, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "Galil AR", 30, 0.09f, 200.0f, 35, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "AK-47", 36, 0.10f, 200.0f, 30, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "M4A4", 33, 0.09f, 200.0f, 30, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "M4A1-S", 38, 0.10f, 200.0f, 20, 80, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "SG 553", 30, 0.11f, 200.0f, 30, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "AUG", 28, 0.10f, 200.0f, 30, 90, 2.50f, 1, 0.006f, 0.030f, 0.055f, 0.025f, 0.0065f, 0.080f, 0.35f, 1.10f, 6.0f },
        { "SSG 08", 88, 1.25f, 300.0f, 10, 90, 2.50f, 1, 0.002f, 0.040f, 0.060f, 0.025f, 0.0100f, 0.060f, 0.50f, 1.10f, 6.0f },
        { "AWP", 115, 1.455f, 300.0f, 5, 30, 2.50f, 1, 0.002f, 0.040f, 0.060f, 0.025f, 0.0100f, 0.060f, 0.50f, 1.10f, 6.0f },
        { "G3SG1", 80, 0.25f, 300.0f, 20, 90, 2.50f, 1, 0.002f, 0.040f, 0.060f, 0.025f, 0.0100f, 0.060f, 0.50f, 1.10f, 6.0f },
        { "SCAR-20", 80, 0.25f, 300.0f, 20, 90, 2.50f, 1, 0.002f, 0.040f, 0.060f, 0.025f, 0.0100f, 0.060f, 0.50f, 1.10f, 6.0f },
        { "Zeus x27", 500, 0.15f, 8.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "HE Grenade", 99, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Flashbang", 0, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Smoke Grenade", 0, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Molotov", 0, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Incendiary", 0, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
        { "Decoy Grenade", 0, 0.80f, 20.0f, 1, 0, 0.50f, 1, 0.010f, 0.010f, 0.010f, 0.010f, 0.0050f, 0.050f, 0.30f, 1.10f, 6.0f },
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
