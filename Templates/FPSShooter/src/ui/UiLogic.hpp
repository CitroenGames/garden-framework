#pragma once
// Pure client UI logic for FPSShooter template (hud_menus.md S1-S6, D10).
// No engine/Rml/network deps: GameplayTests drives this header directly.
// Client never decides buy/team eligibility; it only derives display state
// from replicated values and sends requests. The server re-validates.
#include "shared/CompetitiveLogic.hpp"
#include "shared/WeaponTypes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace UiLogic {

using Competitive::BuyResult;
using Competitive::FpsTeam;
using Competitive::RoundPhase;
using Competitive::TeamSelectResult;

// Scoreboard is an overlay (Tab-hold), never a modal screen (hud_menus S1).
enum class UiScreen : uint8_t { None, MainMenu, Lobby, TeamSelect, Buy, Scoreboard, Pause, Settings, MatchEnd };

struct UiState {
    UiScreen screen = UiScreen::None;
    UiScreen settings_return_to = UiScreen::MainMenu;
    bool scoreboard_held = false;
    std::string notice_text;
    float notice_remaining = 0.0f;
};

inline bool isModal(UiScreen s) { return s != UiScreen::None && s != UiScreen::Scoreboard; }
inline bool cursorWanted(const UiState& s) { return isModal(s.screen); }
inline bool scoreboardVisible(const UiState& s) {
    return s.scoreboard_held || s.screen == UiScreen::MatchEnd;
}

inline bool isLegalTransition(UiScreen from, UiScreen to) {
    using S = UiScreen;
    if (to == S::Scoreboard || from == S::Scoreboard) return false; // overlay only
    if (from == to) return false;
    switch (from) {
    case S::MainMenu: return to == S::Lobby || to == S::Settings || to == S::None;
    case S::Lobby: return to == S::TeamSelect || to == S::MainMenu || to == S::None || to == S::Settings;
    case S::TeamSelect: return to == S::None || to == S::MatchEnd || to == S::MainMenu;
    case S::None: return to == S::Buy || to == S::TeamSelect || to == S::Pause || to == S::MatchEnd || to == S::MainMenu || to == S::Lobby;
    case S::Buy: return to == S::None || to == S::MatchEnd;
    case S::Pause: return to == S::None || to == S::Settings || to == S::MainMenu || to == S::TeamSelect || to == S::MatchEnd;
    case S::Settings: return to == S::MainMenu || to == S::Pause || to == S::None || to == S::Lobby;
    case S::MatchEnd: return to == S::Lobby || to == S::MainMenu || to == S::None;
    default: return false;
    }
}

// Opens a modal screen. Fails (no change) for Scoreboard or illegal moves,
// preserving modal exclusivity: the single screen field holds at most one.
inline bool tryOpen(UiState& s, UiScreen target) {
    if (target == UiScreen::Scoreboard) return false;
    if (s.screen == target) return true;
    if (target == UiScreen::Settings) {
        if (!isLegalTransition(s.screen, target) && s.screen != UiScreen::None) return false;
        s.settings_return_to = s.screen;
        s.screen = target;
        return true;
    }
    if (!isLegalTransition(s.screen, target)) return false;
    s.screen = target;
    return true;
}
inline void closeToHud(UiState& s) { s.screen = UiScreen::None; }
inline void backFromSettings(UiState& s) {
    if (s.screen == UiScreen::Settings) s.screen = s.settings_return_to;
}
// Tab-hold overlay: only over the bare HUD, never over a modal.
inline void setScoreboardHeld(UiState& s, bool held) {
    s.scoreboard_held = held && s.screen == UiScreen::None;
}
inline void showNotice(UiState& s, const std::string& text, float seconds = 2.0f) {
    s.notice_text = text;
    s.notice_remaining = seconds;
}
inline void tickUi(UiState& s, float dt) {
    if (s.notice_remaining > 0.0f) {
        s.notice_remaining -= dt;
        if (s.notice_remaining <= 0.0f) { s.notice_remaining = 0.0f; s.notice_text.clear(); }
    }
}

inline const char* buyResultText(BuyResult r) {
    using R = BuyResult;
    switch (r) {
    case R::Ok: return "Purchased";
    case R::UnknownItem: return "Unknown item";
    case R::InsufficientFunds: return "Insufficient funds";
    case R::WrongTeam: return "Not available for your team";
    case R::OutsideBuyZone: return "Outside buy zone";
    case R::BuyTimeExpired: return "Buy time expired";
    case R::Dead: return "You are dead";
    case R::SlotOccupied: return "Slot occupied";
    case R::AlreadyOwned: return "Already owned";
    case R::PhaseClosed: return "Buy window closed";
    case R::DuplicateRequest: return "Duplicate request";
    case R::CarryLimit: return "Carry limit reached";
    }
    return "Buy failed";
}
inline const char* teamResultText(TeamSelectResult r) {
    using R = TeamSelectResult;
    switch (r) {
    case R::Ok: return "Team joined";
    case R::TeamFull: return "Team is full";
    case R::BothFull: return "Both teams are full";
    case R::PhaseClosed: return "Team change not allowed now";
    case R::InvalidTeam: return "Invalid team";
    case R::SpawnUnavailable: return "No valid spawn is available";
    }
    return "Team change failed";
}

// Rejection: notice only, screen and local team stay exactly as they were.
inline bool applyBuyResult(UiState& s, BuyResult r) {
    if (r == BuyResult::Ok) return true;
    showNotice(s, buyResultText(r));
    return false;
}
inline bool applyTeamResult(UiState& s, TeamSelectResult r, FpsTeam assigned, FpsTeam& local_team) {
    if (r != TeamSelectResult::Ok) { showNotice(s, teamResultText(r)); return false; }
    local_team = assigned;
    s.screen = UiScreen::None;
    s.notice_text.clear(); s.notice_remaining = 0.0f;
    return true;
}

// --- Round clock (hud_menus S2.3) -------------------------------------------
inline std::string formatMmSs(float remaining) {
    int total = static_cast<int>(std::ceil(std::max(0.0f, remaining) - 0.0001f));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}
inline std::string formatPhaseText(RoundPhase phase, bool bomb_planted) {
    using P = RoundPhase;
    if (bomb_planted && phase == P::Live) return "BOMB PLANTED";
    switch (phase) {
    case P::WaitingForPlayers: return "WAITING";
    case P::Warmup: return "WARMUP";
    case P::Freeze: return "FREEZE";
    case P::Live: return "LIVE";
    case P::RoundEnd: return "ROUND END";
    case P::Halftime: return "HALFTIME";
    case P::MatchEnded: return "MATCH END";
    }
    return "";
}
inline std::string formatClock(RoundPhase phase, float remaining, bool bomb_planted) {
    using P = RoundPhase;
    if (bomb_planted && phase == P::Live) return "BOMB PLANTED";
    switch (phase) {
    case P::WaitingForPlayers: return "WAITING";
    case P::Freeze: return "FREEZE " + formatMmSs(remaining);
    case P::Warmup: return "WARMUP " + formatMmSs(remaining);
    case P::Live: return formatMmSs(remaining);
    case P::RoundEnd: return "ROUND END";
    case P::Halftime: return "HALFTIME";
    case P::MatchEnded: return "MATCH END";
    }
    return formatMmSs(remaining);
}

inline bool isBuyWindowOpen(RoundPhase phase, float seconds_into_live,
                             const Competitive::ModeConfig& cfg = Competitive::ModeConfig{}) {
    if (phase == RoundPhase::Warmup || phase == RoundPhase::Freeze) return true;
    if (phase == RoundPhase::Live) return seconds_into_live <= cfg.buy_window_after_live;
    return false;
}

// --- Buy item display state (hud_menus S3) -----------------------------------
// Values match hud_menus: 0 avail 1 owned 2 equipped 3 poor 4 wrong-team 5 locked-phase.
enum class BuyItemState : int { Avail = 0, Owned = 1, Equipped = 2, Poor = 3, WrongTeam = 4, LockedPhase = 5 };

inline bool teamAllowed(const WeaponCatalogDef& def, FpsTeam team) {
    uint8_t bit = team == FpsTeam::Attackers ? kCatalogTeamAttackers
                : team == FpsTeam::Defenders ? kCatalogTeamDefenders : 0;
    return bit != 0 && (def.team_mask & bit) != 0;
}
inline BuyItemState deriveBuyItemState(const WeaponCatalogDef& def, int32_t money, FpsTeam team,
                                       RoundPhase phase, float seconds_into_live,
                                       bool owned, bool equipped,
                                       const Competitive::ModeConfig& cfg = Competitive::ModeConfig{}) {
    if (!isBuyWindowOpen(phase, seconds_into_live, cfg)) return BuyItemState::LockedPhase;
    if (!teamAllowed(def, team)) return BuyItemState::WrongTeam;
    if (equipped) return BuyItemState::Equipped;
    if (owned) return BuyItemState::Owned;
    if (money < def.price) return BuyItemState::Poor;
    return BuyItemState::Avail;
}
inline const char* buyItemStateText(BuyItemState s) {
    switch (s) {
    case BuyItemState::Avail: return "avail";
    case BuyItemState::Owned: return "owned";
    case BuyItemState::Equipped: return "equipped";
    case BuyItemState::Poor: return "poor";
    case BuyItemState::WrongTeam: return "wrong-team";
    case BuyItemState::LockedPhase: return "locked-phase";
    }
    return "?";
}
// Number-key featured buy list (keyboard-first v1): digit 1..9,0 -> index 0..9.
inline const std::array<uint16_t, 10>& featuredBuyIds() {
    static const std::array<uint16_t, 10> ids = {9, 26, 27, 32, 24, 25, 36, 37, 38, 42};
    return ids;
}

// --- Scoreboard row model (hud_menus S5) --------------------------------------
struct ScoreboardRowInput {
    std::string name;
    int32_t kills = 0, assists = 0, deaths = 0, money = 0, ping = 0;
    FpsTeam side = FpsTeam::None;
    bool is_local = false, alive = true, is_bot = false;
};
struct ScoreboardRow {
    std::string display_name;
    int32_t kills = 0, assists = 0, deaths = 0, ping = 0;
    int32_t money_display = -1; // -1 = hidden (enemy money, spec S8)
    bool money_visible = false;
    FpsTeam side = FpsTeam::None;
    bool is_local = false;
};
inline ScoreboardRow buildScoreboardRow(const ScoreboardRowInput& in, FpsTeam viewer_team) {
    ScoreboardRow out;
    out.display_name = in.name + (in.is_bot ? " [BOT]" : "") + (!in.alive ? " [DEAD]" : "");
    out.kills = in.kills; out.assists = in.assists; out.deaths = in.deaths; out.ping = in.ping;
    out.side = in.side; out.is_local = in.is_local;
    out.money_visible = Competitive::isPlayingTeam(viewer_team) && in.side == viewer_team;
    out.money_display = out.money_visible ? in.money : -1;
    return out;
}

// --- Replicated client competitive state --------------------------------------
struct ClientCompetitiveState {
    RoundPhase phase = RoundPhase::WaitingForPlayers;
    float phase_time_remaining = 0.0f;
    float phase_elapsed = 0.0f;
    uint16_t round_number = 0;
    int32_t score_attackers = 0, score_defenders = 0;
    bool bomb_planted = false;
    int32_t money = 800;
    uint8_t loss_streak = 0;
    FpsTeam local_team = FpsTeam::None;
    bool has_round_state = false, has_economy = false;
    uint32_t next_buy_seq = 1;
    int32_t armor = 0;
    bool has_helmet = false, has_kit = false;
};
inline void applyRoundState(ClientCompetitiveState& s, uint8_t phase, float remaining,
                            uint16_t round_number, int32_t wins_a, int32_t wins_d, bool planted) {
    RoundPhase p = static_cast<RoundPhase>(phase);
    if (!s.has_round_state || p != s.phase) s.phase_elapsed = 0.0f;
    s.phase = p; s.phase_time_remaining = remaining; s.round_number = round_number;
    s.score_attackers = wins_a; s.score_defenders = wins_d; s.bomb_planted = planted;
    s.has_round_state = true;
}
inline void applyEconomyState(ClientCompetitiveState& s, int32_t balance, uint8_t loss_streak) {
    s.money = balance; s.loss_streak = loss_streak; s.has_economy = true;
}
inline void tickCompetitive(ClientCompetitiveState& s, float dt) {
    s.phase_elapsed += dt;
    if (s.phase_time_remaining > 0.0f) s.phase_time_remaining = std::max(0.0f, s.phase_time_remaining - dt);
}
inline bool buyWindowOpen(const ClientCompetitiveState& s,
                          const Competitive::ModeConfig& cfg = Competitive::ModeConfig{}) {
    // Live elapsed is tracked locally from phase entry; the server deadline
    // stays authoritative and re-validates every request.
    return isBuyWindowOpen(s.phase, s.phase_elapsed, cfg);
}
// Buy/TeamSelect exist only in competitive modes: without replicated round
// state (DM/TDM) B/M must not open them. Display gate only; server validates.
inline bool canOpenBuy(const ClientCompetitiveState& s,
                       const Competitive::ModeConfig& cfg = Competitive::ModeConfig{}) {
    return s.has_round_state && buyWindowOpen(s, cfg);
}
inline bool canOpenTeamSelect(const ClientCompetitiveState& s) { return s.has_round_state; }

// --- Core HUD source map (hud_menus U1) --------------------------------------
// The HUD is a client presentation of sources that were already supplied by
// authority.  This deliberately carries no locally predicted health, ammo, or
// phase values: a disconnected client, a client without a possessed pawn, and
// a client without an owner-targeted snapshot all render unavailable data.
struct HudSourceState {
    bool connected = false;
    bool has_local_pawn = false;

    // Replicated local-pawn health, when that component is available to the
    // client.  The damage/respawn snapshot is the existing authoritative
    // fallback for clients whose local entity has no FpsHealthComponent.
    const FpsHealthComponent* pawn_health = nullptr;
    bool has_damage_snapshot = false;
    FpsHealthComponent damage_snapshot{};

    // WEAPON_STATE is owner-targeted on the existing combat protocol.
    bool has_weapon_snapshot = false;
    WeaponComponent weapon_snapshot{};

    // ROUND_STATE is a shared server snapshot.  A missing snapshot is not a
    // local "waiting" decision; it stays unavailable until the server sends it.
    const ClientCompetitiveState* competitive = nullptr;
};

struct HudCoreBindings {
    bool connected = false;
    std::string connection_text = "DISCONNECTED";
    bool health_available = false;
    int32_t health = 0;
    int32_t max_health = 0;
    std::string health_text = "UNAVAILABLE";
    bool alive = false;
    bool weapon_available = false;
    std::string weapon_name = "UNAVAILABLE";
    std::string ammo_text = "UNAVAILABLE";
    std::string reserve_text = "UNAVAILABLE";
    bool reloading = false;
    bool phase_available = false;
    std::string phase_text = "UNAVAILABLE";
    std::string clock_text;
    std::string round_text;
    int32_t score_attackers = 0;
    int32_t score_defenders = 0;
    bool bomb_planted = false;
};

inline HudCoreBindings deriveHudCoreBindings(const HudSourceState& source)
{
    HudCoreBindings out;
    out.connected = source.connected;
    if (!source.connected)
        return out;

    out.connection_text = source.has_local_pawn ? "CONNECTED" : "AWAITING PLAYER";
    if (!source.has_local_pawn)
        return out;

    // DAMAGE_EVENT/PLAYER_RESPAWN are the newest owner-targeted health
    // reports, so they must supersede an older local ECS copy.
    const FpsHealthComponent* health = source.has_damage_snapshot
        ? &source.damage_snapshot
        : source.pawn_health;
    if (health) {
        out.health_available = true;
        out.health = std::max(0, health->health);
        out.max_health = std::max(0, health->max_health);
        out.health_text = std::to_string(out.health) + " / " + std::to_string(out.max_health);
        out.alive = health->alive;
    }

    if (source.has_weapon_snapshot && hasWeaponDefinition(source.weapon_snapshot.weapon_type)) {
        const WeaponComponent& weapon = source.weapon_snapshot;
        const WeaponDef& def = getWeaponDef(weapon.weapon_type);
        out.weapon_available = true;
        out.weapon_name = def.name;
        out.reloading = weapon.reloading;
        if (weapon.reloading) {
            out.ammo_text = "RELOADING";
        } else {
            out.ammo_text = std::to_string(std::max(0, weapon.ammo)) + " / " +
                            std::to_string(std::max(0, weapon.max_ammo));
        }
        out.reserve_text = std::to_string(std::max(0, weapon.reserve_ammo)) + " / " +
                           std::to_string(std::max(0, weapon.max_reserve_ammo));
    }

    if (source.competitive && source.competitive->has_round_state) {
        const ClientCompetitiveState& comp = *source.competitive;
        out.phase_available = true;
        out.phase_text = formatPhaseText(comp.phase, comp.bomb_planted);
        out.clock_text = formatClock(comp.phase, comp.phase_time_remaining, comp.bomb_planted);
        out.round_text = "Round " + std::to_string(comp.round_number);
        out.score_attackers = comp.score_attackers;
        out.score_defenders = comp.score_defenders;
        out.bomb_planted = comp.bomb_planted;
    }
    return out;
}

// --- Radar D10: fixed slots, north-up, teammates only --------------------------
constexpr int kRadarSlots = 10;
struct RadarBlip { bool visible = false; std::string x = "50.0%", y = "50.0%"; };
inline RadarBlip projectRadarBlip(float wx, float wz, float origin_x, float origin_z,
                                  float world_size, bool alive) {
    RadarBlip out;
    if (!alive || world_size <= 0.0f) return out;
    float u = (wx - origin_x) / world_size;
    float v = (wz - origin_z) / world_size;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return out; // off-map hides
    char xb[16], yb[16];
    std::snprintf(xb, sizeof(xb), "%.1f%%", u * 100.0f);
    std::snprintf(yb, sizeof(yb), "%.1f%%", v * 100.0f);
    out.visible = true; out.x = xb; out.y = yb;
    return out;
}
struct RadarThrottle {
    float accumulator = 1.0f; // first update immediate
    bool shouldUpdate(float dt) {
        accumulator += dt;
        if (accumulator >= 0.1f) { accumulator = 0.0f; return true; } // 10 Hz
        return false;
    }
};

} // namespace UiLogic
