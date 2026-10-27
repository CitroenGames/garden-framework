#pragma once
// FPSShooter game-layer logic (template only, no EngineCore changes):
// team spawns, inventory-based weapons, DM/TDM match rules.
// Pure helpers so Tests/GameplayTests can exercise them without a server.
#include "SharedComponents.hpp"
#include "WeaponTypes.hpp"
#include "WeaponSystem.hpp"
// Engine gameplay primitives (Team/Inventory/Wallet/Timer/Channel). The
// template health type is FpsHealthComponent, so no name collision remains.
#include "Gameplay/GameplayComponents.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace FpsLogic {

// Engine TeamComponent team_id values (game-defined mapping).
constexpr uint8_t kTeamNone = 0;
constexpr uint8_t kTeamAttackers = 1;
constexpr uint8_t kTeamDefenders = 2;

inline uint32_t itemDefId(WeaponType t) { return static_cast<uint32_t>(t) + 1u; }
inline bool weaponTypeFromItemDef(uint32_t id, WeaponType& out) {
    if (id == 0 || id - 1 >= static_cast<uint32_t>(WeaponType::COUNT)) return false;
    out = static_cast<WeaponType>(id - 1); return true;
}
inline WeaponSlot slotForWeaponType(WeaponType t) {
    switch (t) {
    case WeaponType::PISTOL: case WeaponType::GLOCK: case WeaponType::P2000:
    case WeaponType::USP_S: case WeaponType::P250: case WeaponType::TEC9:
    case WeaponType::FIVESEVEN: case WeaponType::CZ75: case WeaponType::ELITE:
    case WeaponType::DEAGLE: case WeaponType::REVOLVER:
        return WeaponSlot::Secondary;
    case WeaponType::KNIFE: return WeaponSlot::Melee;
    case WeaponType::GRENADE: case WeaponType::HEGRENADE: case WeaponType::FLASHBANG:
    case WeaponType::SMOKEGRENADE: case WeaponType::MOLOTOV: case WeaponType::INCENDIARY:
    case WeaponType::DECOY:
        return WeaponSlot::Grenade;
    case WeaponType::TASER: return WeaponSlot::Utility;
    default: return WeaponSlot::Primary; // RIFLE/SHOTGUN + all catalog primaries
    }
}

inline bool isValidWeaponType(WeaponType type)
{
    return static_cast<uint8_t>(type) < static_cast<uint8_t>(WeaponType::COUNT);
}

inline bool isValidLoadoutSlot(int slot)
{
    return slot >= 0 && slot < static_cast<int>(WeaponSlot::COUNT);
}

inline bool isValidSlotState(const WeaponSlotState& state, int slot)
{
    if (!state.occupied)
        return state.ammo == 0 && state.reserve == 0;
    if (!isValidLoadoutSlot(slot) || !isValidWeaponType(state.type) ||
        static_cast<int>(slotForWeaponType(state.type)) != slot)
        return false;
    const WeaponDef& def = getWeaponDef(state.type);
    return state.ammo >= 0 && state.ammo <= def.max_ammo &&
           state.reserve >= 0 && state.reserve <= def.max_reserve_ammo;
}

inline void normalizeSlotState(WeaponSlotState& state, int slot)
{
    if (!state.occupied) {
        state = WeaponSlotState{};
        return;
    }
    if (!isValidLoadoutSlot(slot) || !isValidWeaponType(state.type) ||
        static_cast<int>(slotForWeaponType(state.type)) != slot) {
        state = WeaponSlotState{};
        return;
    }
    const WeaponDef& def = getWeaponDef(state.type);
    state.ammo = std::clamp(state.ammo, 0, def.max_ammo);
    state.reserve = std::clamp(state.reserve, 0, def.max_reserve_ammo);
}

inline void normalizeLoadout(PlayerLoadoutComponent& lo)
{
    int first_occupied = -1;
    for (int slot = 0; slot < static_cast<int>(WeaponSlot::COUNT); ++slot) {
        normalizeSlotState(lo.slots[slot], slot);
        if (first_occupied < 0 && lo.slots[slot].occupied)
            first_occupied = slot;
    }
    if (!isValidLoadoutSlot(lo.active_slot) || !lo.slots[lo.active_slot].occupied)
        lo.active_slot = first_occupied;
}

// ---- Inventory / loadout ----
inline void saveActiveAmmo(PlayerLoadoutComponent& lo, const WeaponComponent& w) {
    if (!isValidLoadoutSlot(lo.active_slot)) return;
    auto& s = lo.slots[lo.active_slot];
    if (s.occupied && s.type == w.weapon_type) {
        s.ammo = w.ammo;
        s.reserve = w.reserve_ammo;
        normalizeSlotState(s, lo.active_slot);
    }
}
inline bool loadSlotIntoWeapon(PlayerLoadoutComponent& lo, WeaponComponent& w, int slot) {
    normalizeLoadout(lo);
    if (!isValidLoadoutSlot(slot) || !lo.slots[slot].occupied)
        return false;
    const auto& s = lo.slots[slot];
    if (!initWeapon(w, s.type))
        return false;
    w.ammo = s.ammo; w.reserve_ammo = s.reserve;
    w.max_ammo = getWeaponDef(s.type).max_ammo;
    w.max_reserve_ammo = getWeaponDef(s.type).max_reserve_ammo;
    return true;
}
inline void syncInventory(InventoryComponent& inv, PlayerLoadoutComponent& lo) {
    normalizeLoadout(lo);
    for (auto& slot : inv.slots)
        slot = InventorySlot{};
    for (size_t i = 0; i < (size_t)WeaponSlot::COUNT; ++i) {
        auto& dst = inv.slots[i];
        if (lo.slots[i].occupied) { dst.item_def_id = itemDefId(lo.slots[i].type); dst.count = 1; }
    }
    inv.active_slot = lo.active_slot;
}
inline bool hasMirroredLoadoutInventory(const InventoryComponent& inv, const PlayerLoadoutComponent& lo) {
    if (inv.active_slot != lo.active_slot || !isValidLoadoutSlot(lo.active_slot) ||
        !lo.slots[lo.active_slot].occupied)
        return false;
    for (size_t i = 0; i < static_cast<size_t>(WeaponSlot::COUNT); ++i) {
        if (!isValidSlotState(lo.slots[i], static_cast<int>(i)))
            return false;
        const InventorySlot& inventory_slot = inv.slots[i];
        if (lo.slots[i].occupied) {
            if (inventory_slot.item_def_id != itemDefId(lo.slots[i].type) || inventory_slot.count != 1)
                return false;
        } else if (!inventory_slot.empty()) {
            return false;
        }
    }
    for (size_t i = static_cast<size_t>(WeaponSlot::COUNT); i < inv.slots.size(); ++i)
        if (!inv.slots[i].empty())
            return false;
    return true;
}
inline void grantLoadout(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w, WeaponType primary = WeaponType::RIFLE) {
    for (auto& s : lo.slots) s = WeaponSlotState{};
    auto give = [&](WeaponSlot slot, WeaponType t) {
        auto& s = lo.slots[(size_t)slot]; const auto& d = getWeaponDef(t);
        s.type = t; s.ammo = d.max_ammo; s.reserve = d.max_reserve_ammo; s.occupied = true;
    };
    give(WeaponSlot::Primary, isValidWeaponType(primary) ? primary : WeaponType::RIFLE);
    give(WeaponSlot::Secondary, WeaponType::PISTOL);
    give(WeaponSlot::Melee, WeaponType::KNIFE);
    lo.active_slot = (int)WeaponSlot::Primary;
    loadSlotIntoWeapon(lo, w, lo.active_slot);
    syncInventory(inv, lo);
}
// Give a weapon into its slot if free (used for shotgun reachability / pickups of new slot).
inline bool giveWeapon(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w, WeaponType t) {
    if (!isValidWeaponType(t)) return false;
    normalizeLoadout(lo);
    int slot = (int)slotForWeaponType(t);
    if (lo.slots[slot].occupied) return false;
    const auto& d = getWeaponDef(t);
    lo.slots[slot] = WeaponSlotState{t, d.max_ammo, d.max_reserve_ammo, true};
    syncInventory(inv, lo); (void)w; return true;
}
inline bool switchToSlot(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w, int slot) {
    normalizeLoadout(lo);
    if (slot < 0 || slot >= (int)WeaponSlot::COUNT) return false;
    if (!lo.slots[slot].occupied) return false;
    WeaponSystem::cancelReload(w);
    if (slot == lo.active_slot) {
        loadSlotIntoWeapon(lo, w, slot);
        syncInventory(inv, lo);
        return true;
    }
    saveActiveAmmo(lo, w);
    lo.active_slot = slot;
    loadSlotIntoWeapon(lo, w, slot);
    syncInventory(inv, lo);
    return true;
}
// D3 WEAPON_SELECT: 1=Primary 2=Secondary 3=Melee 4=Grenade 5=Utility(C4 in plan)
inline bool switchByWeaponSelect(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w, uint8_t sel) {
    if (sel < 1 || sel > 5) return false;
    return switchToSlot(inv, lo, w, (int)sel - 1);
}
inline WeaponType activeWeaponType(const PlayerLoadoutComponent& lo, const WeaponComponent& w) {
    if (lo.active_slot >= 0 && lo.active_slot < (int)WeaponSlot::COUNT && lo.slots[lo.active_slot].occupied)
        return lo.slots[lo.active_slot].type;
    return w.weapon_type;
}
struct DroppedItem { WeaponType type; int32_t ammo; int32_t reserve; };
// DROP: active Primary/Secondary/Grenade/Utility drops; Melee never droppable.
inline std::optional<DroppedItem> dropActive(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w) {
    normalizeLoadout(lo);
    int slot = lo.active_slot;
    if (slot < 0 || slot >= (int)WeaponSlot::COUNT) return std::nullopt;
    if (slot == (int)WeaponSlot::Melee) return std::nullopt;
    if (!lo.slots[slot].occupied) return std::nullopt;
    WeaponSystem::cancelReload(w);
    saveActiveAmmo(lo, w);
    DroppedItem out{lo.slots[slot].type, lo.slots[slot].ammo, lo.slots[slot].reserve};
    lo.slots[slot] = WeaponSlotState{};
    // Fall back: Primary -> Secondary -> Melee -> any occupied.
    int fallback = -1;
    const int order[] = {(int)WeaponSlot::Primary, (int)WeaponSlot::Secondary, (int)WeaponSlot::Melee, (int)WeaponSlot::Grenade, (int)WeaponSlot::Utility};
    for (int c : order) if (lo.slots[c].occupied) { fallback = c; break; }
    lo.active_slot = fallback;
    if (fallback >= 0) loadSlotIntoWeapon(lo, w, fallback);
    syncInventory(inv, lo);
    return out;
}
// Walk-over pickup only if the weapon's slot is free.
inline bool tryPickup(InventoryComponent& inv, PlayerLoadoutComponent& lo, WeaponComponent& w, const DroppedItem& item) {
    if (!isValidWeaponType(item.type)) return false;
    normalizeLoadout(lo);
    int slot = (int)slotForWeaponType(item.type);
    if (lo.slots[slot].occupied) return false;
    lo.slots[slot] = WeaponSlotState{item.type, item.ammo, item.reserve, true};
    if (!isValidSlotState(lo.slots[slot], slot)) {
        lo.slots[slot] = WeaponSlotState{};
        return false;
    }
    if (lo.active_slot < 0) { lo.active_slot = slot; loadSlotIntoWeapon(lo, w, slot); }
    syncInventory(inv, lo); return true;
}

// ---- Spawn selection ----
struct TeamSpawnPoint { glm::vec3 position{0,5,0}; uint8_t team_id = kTeamNone; };
inline size_t selectFarthestSpawnIndex(const std::vector<glm::vec3>& cands, const std::vector<glm::vec3>& enemies) {
    if (cands.empty() || enemies.empty()) return 0;
    size_t best = 0; float bestScore = -1.0f;
    for (size_t i = 0; i < cands.size(); ++i) {
        float mn = 1e30f;
        for (auto& e : enemies) mn = std::min(mn, glm::distance(cands[i], e));
        if (mn > bestScore) { bestScore = mn; best = i; }
    }
    return best;
}

// ---- DM / TDM match logic ----
enum class FpsGameMode : uint8_t { Deathmatch = 0, TeamDeathmatch = 1 };
struct ModeConfig {
    FpsGameMode mode = FpsGameMode::Deathmatch;
    int32_t score_limit = 25;          // DM: frags; TDM: team frags (plan: 100, configurable)
    float time_limit_seconds = 600.0f;
    float respawn_delay = 3.0f;
    float spawn_protection_seconds = 1.5f;
    float match_end_seconds = 5.0f;
};
inline ModeConfig deathmatchConfig() { ModeConfig c; c.mode = FpsGameMode::Deathmatch; c.score_limit = 25; return c; }
inline ModeConfig teamDeathmatchConfig() { ModeConfig c; c.mode = FpsGameMode::TeamDeathmatch; c.score_limit = 100; return c; }

struct MatchLogic {
    ModeConfig config;
    std::unordered_map<uint16_t, int32_t> player_scores;
    std::array<int32_t, 3> team_scores{{0,0,0}};
    float elapsed = 0.0f;
    float end_timer = 0.0f;
    bool ended = false;
    uint16_t winner_client = 0; // DM winner; 0 = none/draw
    uint8_t winner_team = kTeamNone;
    void configure(const ModeConfig& c) { *this = MatchLogic{}; config = c; }
    void restart() { player_scores.clear(); team_scores = {{0,0,0}}; elapsed = 0; end_timer = 0; ended = false; winner_client = 0; winner_team = kTeamNone; }
    void recordKill(uint16_t killer, uint16_t victim, uint8_t killer_team) {
        if (ended || killer == 0) return;
        if (killer != victim) {
            player_scores[killer]++;
            if (config.mode == FpsGameMode::TeamDeathmatch && killer_team < team_scores.size()) team_scores[killer_team]++;
        }
        checkEnd();
    }
    void checkEnd() {
        if (ended) return;
        if (config.mode == FpsGameMode::Deathmatch) {
            for (auto& kv : player_scores) if (kv.second >= config.score_limit) { ended = true; winner_client = kv.first; end_timer = config.match_end_seconds; return; }
        } else {
            for (uint8_t t = kTeamAttackers; t <= kTeamDefenders; ++t) if (team_scores[t] >= config.score_limit) { ended = true; winner_team = t; end_timer = config.match_end_seconds; return; }
        }
        if (config.time_limit_seconds > 0 && elapsed >= config.time_limit_seconds) {
            ended = true; end_timer = config.match_end_seconds;
            if (config.mode == FpsGameMode::Deathmatch) {
                int32_t best = -1; for (auto& kv : player_scores) if (kv.second > best) { best = kv.second; winner_client = kv.first; }
            } else winner_team = team_scores[kTeamAttackers] > team_scores[kTeamDefenders] ? kTeamAttackers : (team_scores[kTeamDefenders] > team_scores[kTeamAttackers] ? kTeamDefenders : kTeamNone);
        }
    }
    // Returns true once when the post-match timer expires (caller restarts).
    bool tick(float dt) {
        if (dt <= 0) return false;
        if (!ended) { elapsed += dt; checkEnd(); return false; }
        end_timer -= dt; return end_timer <= 0.0f;
    }
    int32_t scoreOf(uint16_t c) const { auto it = player_scores.find(c); return it == player_scores.end() ? 0 : it->second; }
};

} // namespace FpsLogic
