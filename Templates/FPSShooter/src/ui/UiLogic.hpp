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

    // Owner-only BUY_RESULT / TEAM_RESULT are retained as presentation data.
    // These fields never authorize a purchase or team change; the server has
    // already made that decision before the client sees them.
    struct BuyReply {
        bool received = false;
        uint16_t item_id = 0;
        BuyResult result = BuyResult::UnknownItem;
        std::string item_name = "UNAVAILABLE";
        std::string text;
        bool accepted = false;
    } buy_reply;
    struct TeamReply {
        bool received = false;
        TeamSelectResult result = TeamSelectResult::InvalidTeam;
        FpsTeam assigned = FpsTeam::None;
        std::string text;
        bool accepted = false;
    } team_reply;
};

// The module owns its network client.  UI only observes the connection edge
// and selects a neutral presentation after an established connection drops;
// it neither initiates a connection nor attempts to repair one.
struct ConnectionLifecycle {
    bool observed_connected = false;
};

inline bool observeConnection(ConnectionLifecycle& lifecycle, UiState& state, bool connected) {
    const bool disconnected = lifecycle.observed_connected && !connected;
    lifecycle.observed_connected = connected;
    if (!disconnected)
        return false;

    // A stale gameplay modal must not keep suppressing input after its owning
    // connection is gone.  The persistent HUD document remains the presenter;
    // MainMenu is a neutral offline surface, not a connection command.
    state.screen = UiScreen::MainMenu;
    state.scoreboard_held = false;
    state.notice_text = "Connection lost";
    state.notice_remaining = 2.0f;
    return true;
}

inline void resetConnectionLifecycle(ConnectionLifecycle& lifecycle) {
    lifecycle.observed_connected = false;
}

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

// One game-context Rml document is verified today.  These values describe
// panels inside that document, so moving between screens never closes or
// reloads the always-on HUD or its data model.
struct SettingsPresentation {
    bool vsync = false;
    bool fxaa = false;
    float sensitivity = 0.0f;
};

struct DocumentPresentation {
    bool hud_document_visible = true;
    bool front_end_visible = false;
    bool settings_visible = false;
    std::string front_end_text;
    std::string settings_text;
};

inline std::string formatSettingsText(const SettingsPresentation& settings) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer),
                  "SETTINGS (read-only)\\nVSync: %s\\nFXAA: %s\\nSensitivity: %.1f\\nUse console settings; archived values save on exit.",
                  settings.vsync ? "ON" : "OFF",
                  settings.fxaa ? "ON" : "OFF",
                  std::max(0.0f, settings.sensitivity));
    return buffer;
}

inline DocumentPresentation buildDocumentPresentation(const UiState& state,
                                                       const SettingsPresentation& settings) {
    DocumentPresentation out;
    switch (state.screen) {
    case UiScreen::MainMenu:
        out.front_end_visible = true;
        out.front_end_text = "MAIN MENU\\nConnection is unavailable.";
        break;
    case UiScreen::Lobby:
        out.front_end_visible = true;
        out.front_end_text = "LOBBY\\nConnection flow is unavailable.";
        break;
    case UiScreen::Settings:
        out.settings_visible = true;
        out.settings_text = formatSettingsText(settings);
        break;
    default:
        break;
    }
    return out;
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

inline const char* teamDisplayName(FpsTeam team) {
    switch (team) {
    case FpsTeam::Attackers: return "Attackers";
    case FpsTeam::Defenders: return "Defenders";
    case FpsTeam::Spectator: return "Spectator";
    default: return "UNAVAILABLE";
    }
}

inline bool isKnownBuyResult(BuyResult result) {
    switch (result) {
    case BuyResult::Ok:
    case BuyResult::UnknownItem:
    case BuyResult::InsufficientFunds:
    case BuyResult::WrongTeam:
    case BuyResult::OutsideBuyZone:
    case BuyResult::BuyTimeExpired:
    case BuyResult::Dead:
    case BuyResult::SlotOccupied:
    case BuyResult::AlreadyOwned:
    case BuyResult::PhaseClosed:
    case BuyResult::DuplicateRequest:
    case BuyResult::CarryLimit:
        return true;
    }
    return false;
}

inline bool isKnownTeamResult(TeamSelectResult result) {
    switch (result) {
    case TeamSelectResult::Ok:
    case TeamSelectResult::TeamFull:
    case TeamSelectResult::BothFull:
    case TeamSelectResult::PhaseClosed:
    case TeamSelectResult::InvalidTeam:
    case TeamSelectResult::SpawnUnavailable:
        return true;
    }
    return false;
}

inline bool isPublishedTeam(FpsTeam team) {
    return team == FpsTeam::Attackers || team == FpsTeam::Defenders || team == FpsTeam::Spectator;
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

// --- Scoreboard row model (hud_menus U3) --------------------------------------
//
// The game protocol currently publishes only a shared ROUND_STATE and owner
// snapshots (team and wallet).  It does not publish a roster.  Keep the
// presentation formatter able to consume a future *approved* roster, but do
// not populate it from replicated world entities: their side, stats, names,
// wallet and liveness are not an approved scoreboard source.
constexpr size_t kScoreboardMaxRows = 10;
constexpr size_t kScoreboardNameMaxChars = 24;

struct ScoreboardRowInput {
    std::string name;
    int32_t kills = 0, assists = 0, deaths = 0, money = 0, ping = 0;
    FpsTeam side = FpsTeam::None;
    bool is_local = false, alive = true, is_bot = false;
    // Assists and per-player ping have no approved payload today.  A row may
    // contain them only when its approved source explicitly says it has them.
    bool assists_available = false;
    bool ping_available = false;
};
struct ScoreboardRow {
    std::string display_name;
    int32_t kills = 0, assists = 0, deaths = 0, ping = 0;
    int32_t money_display = -1; // -1 = hidden (enemy money, spec S8)
    bool money_visible = false, assists_available = false, ping_available = false;
    FpsTeam side = FpsTeam::None;
    bool is_local = false;
};

inline std::string scoreboardDisplayName(const std::string& source_name) {
    std::string out;
    out.reserve(std::min(source_name.size(), kScoreboardNameMaxChars));
    for (const unsigned char ch : source_name) {
        if (out.size() == kScoreboardNameMaxChars)
            break;
        // Rml receives a flat text field.  Keep untrusted display names on one
        // row and leave markup escaping to Rml's data-model binding.
        out += (ch >= 32 && ch != 127) ? static_cast<char>(ch) : ' ';
    }
    return out.empty() ? "PLAYER" : out;
}

inline ScoreboardRow buildScoreboardRow(const ScoreboardRowInput& in, FpsTeam viewer_team) {
    ScoreboardRow out;
    out.display_name = scoreboardDisplayName(in.name) + (in.is_bot ? " [BOT]" : "") + (!in.alive ? " [DEAD]" : "");
    out.kills = std::max(0, in.kills);
    out.assists = std::max(0, in.assists);
    out.deaths = std::max(0, in.deaths);
    out.ping = std::max(0, in.ping);
    out.assists_available = in.assists_available;
    out.ping_available = in.ping_available;
    out.side = in.side; out.is_local = in.is_local;
    out.money_visible = Competitive::isPlayingTeam(viewer_team) && in.side == viewer_team;
    out.money_display = out.money_visible ? std::max(0, in.money) : -1;
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
    // Retained from the reliable ROUND_STATE snapshot.  Objective acceptance
    // actions use this only to identify the authoritative round resolution.
    Competitive::RoundEndReason last_round_end_reason = Competitive::RoundEndReason::None;
    // ROUND_STATE provides this pair.  It is retained for ordering and for
    // display correction; client time is only an interpolation between these
    // authoritative snapshots.
    uint32_t phase_deadline_tick = 0;
    uint32_t round_server_tick = 0;
    bool round_clock_corrected = false;
    int32_t money = 800;
    uint8_t loss_streak = 0;
    FpsTeam local_team = FpsTeam::None;
    bool has_round_state = false, has_economy = false;
    uint32_t next_buy_seq = 1;
    int32_t armor = 0;
    bool has_helmet = false, has_kit = false;
};

inline bool isKnownRoundPhase(RoundPhase phase) {
    using P = RoundPhase;
    switch (phase) {
    case P::WaitingForPlayers:
    case P::Warmup:
    case P::Freeze:
    case P::Live:
    case P::RoundEnd:
    case P::Halftime:
    case P::MatchEnded:
        return true;
    }
    return false;
}

inline bool isRoundStateAtLeastAsNew(uint32_t incoming_tick, uint32_t accepted_tick) {
    // Standard wrapped tick ordering.  The protocol transport is reliable, but
    // retaining this guard prevents a delayed custom-message path from making
    // the displayed phase/clock regress.
    return static_cast<int32_t>(incoming_tick - accepted_tick) >= 0;
}

inline bool applyRoundState(ClientCompetitiveState& s, uint8_t phase, float remaining,
                            uint16_t round_number, int32_t wins_a, int32_t wins_d, bool planted,
                            uint32_t phase_deadline_tick = 0, uint32_t server_tick = 0,
                            uint8_t last_round_end_reason = 0) {
    RoundPhase p = static_cast<RoundPhase>(phase);
    if (!isKnownRoundPhase(p) || !std::isfinite(remaining))
        return false;
    if (s.has_round_state && !isRoundStateAtLeastAsNew(server_tick, s.round_server_tick))
        return false;

    const float corrected_remaining = std::max(0.0f, remaining);
    const bool phase_changed = !s.has_round_state || p != s.phase;
    // tickCompetitive decrements the prior authoritative value between
    // messages.  A non-trivial replacement is a server clock correction, not
    // a local phase transition.
    s.round_clock_corrected = s.has_round_state && !phase_changed &&
        std::fabs(s.phase_time_remaining - corrected_remaining) > 0.2f;
    if (phase_changed) s.phase_elapsed = 0.0f;
    s.phase = p; s.phase_time_remaining = corrected_remaining; s.round_number = round_number;
    s.score_attackers = std::max(0, wins_a); s.score_defenders = std::max(0, wins_d); s.bomb_planted = planted;
    s.last_round_end_reason = static_cast<Competitive::RoundEndReason>(last_round_end_reason);
    s.phase_deadline_tick = phase_deadline_tick;
    s.round_server_tick = server_tick;
    s.has_round_state = true;
    return true;
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

// Flat text is used because the game-context Rml API exposes scalar bindings,
// not collection binding.  `has_authoritative_roster` is deliberately
// separate from `rows`: a local row is useful feedback, but it never claims a
// complete player list in the absence of a roster protocol message.
struct ScoreboardPresentationSource {
    bool connected = false;
    const ClientCompetitiveState* competitive = nullptr;
    bool has_authoritative_roster = false;
    std::vector<ScoreboardRowInput> approved_rows;
};

struct ScoreboardPresentation {
    bool available = false;
    bool roster_available = false;
    bool roster_truncated = false;
    std::array<ScoreboardRow, kScoreboardMaxRows> rows{};
    size_t row_count = 0;
    std::string text = "SCOREBOARD UNAVAILABLE";
};

inline void appendScoreboardRowText(std::string& text, const ScoreboardRow& row) {
    text += teamDisplayName(row.side);
    text += " ";
    text += row.display_name;
    text += " K" + std::to_string(row.kills);
    text += " D" + std::to_string(row.deaths);
    text += row.assists_available ? " A" + std::to_string(row.assists) : " A-";
    text += row.ping_available ? " P" + std::to_string(row.ping) : " P-";
    text += row.money_visible ? " $" + std::to_string(row.money_display) : " $-";
}

inline ScoreboardPresentation buildScoreboardPresentation(const ScoreboardPresentationSource& source) {
    ScoreboardPresentation out;
    if (!source.connected || !source.competitive || !source.competitive->has_round_state)
        return out;

    const ClientCompetitiveState& comp = *source.competitive;
    out.available = true;
    out.roster_available = source.has_authoritative_roster;
    out.text = "Round " + std::to_string(comp.round_number) +
        " ATK " + std::to_string(comp.score_attackers) +
        " - " + std::to_string(comp.score_defenders) + " DEF\n" +
        formatPhaseText(comp.phase, comp.bomb_planted) + " " +
        formatClock(comp.phase, comp.phase_time_remaining, false);
    if (comp.bomb_planted)
        out.text += "\nOBJECTIVE: BOMB PLANTED";

    for (const ScoreboardRowInput& input : source.approved_rows) {
        if (out.row_count == kScoreboardMaxRows) {
            out.roster_truncated = true;
            break;
        }
        // Invalid/non-playing sides do not become a plausible team entry.
        if (!isPublishedTeam(input.side))
            continue;
        const ScoreboardRow row = buildScoreboardRow(input, comp.local_team);
        out.rows[out.row_count++] = row;
        out.text += "\n";
        appendScoreboardRowText(out.text, row);
    }

    if (!out.roster_available)
        out.text += "\nROSTER UNAVAILABLE";
    else if (out.roster_truncated)
        out.text += "\nROSTER TRUNCATED";
    else if (out.row_count == 0)
        out.text += "\nNO PLAYERS";
    return out;
}

// The result payload includes a server-owned wallet balance. Applying that
// value is a presentation refresh, never a local price calculation. Catalog
// data only supplies a label for the numeric item id carried by BUY_RESULT.
inline bool applyAuthoritativeBuyReply(UiState& ui, ClientCompetitiveState& competitive,
                                       uint16_t item_id, BuyResult result,
                                       int32_t authoritative_balance) {
    if (!isKnownBuyResult(result)) {
        ui.buy_reply = {};
        ui.buy_reply.received = true;
        ui.buy_reply.item_id = item_id;
        ui.buy_reply.text = "Buy response unavailable";
        showNotice(ui, ui.buy_reply.text);
        return false;
    }

    const WeaponCatalogDef* item = findCatalogById(item_id);
    ui.buy_reply.received = true;
    ui.buy_reply.item_id = item_id;
    ui.buy_reply.result = result;
    ui.buy_reply.item_name = item ? item->name : "UNAVAILABLE";
    ui.buy_reply.accepted = result == BuyResult::Ok;

    competitive.money = authoritative_balance;
    competitive.has_economy = true;

    if (result == BuyResult::Ok) {
        ui.buy_reply.text = item
            ? "Purchased " + ui.buy_reply.item_name
            : "Purchase confirmed";
    } else if (item) {
        ui.buy_reply.text = ui.buy_reply.item_name + ": " + buyResultText(result);
    } else {
        // A rejected unknown numeric id must not be made to look like a
        // catalog row. It stays a neutral server-result presentation.
        ui.buy_reply.text = buyResultText(result);
    }
    showNotice(ui, ui.buy_reply.text);
    return ui.buy_reply.accepted;
}

// TEAM_RESULT publishes the server's durable assignment. A malformed OK
// reply is presentation failure, so it cannot close the menu or mutate the
// local side display.
inline bool applyAuthoritativeTeamReply(UiState& ui, TeamSelectResult result,
                                        FpsTeam assigned, FpsTeam& local_team) {
    if (!isKnownTeamResult(result) || (result == TeamSelectResult::Ok && !isPublishedTeam(assigned))) {
        ui.team_reply = {};
        ui.team_reply.received = true;
        ui.team_reply.assigned = assigned;
        ui.team_reply.text = "Team response unavailable";
        showNotice(ui, ui.team_reply.text);
        return false;
    }

    ui.team_reply.received = true;
    ui.team_reply.result = result;
    ui.team_reply.assigned = assigned;
    ui.team_reply.accepted = result == TeamSelectResult::Ok;
    ui.team_reply.text = result == TeamSelectResult::Ok
        ? std::string("Joined ") + teamDisplayName(assigned)
        : teamResultText(result);

    if (!applyTeamResult(ui, result, assigned, local_team))
        return false;
    showNotice(ui, ui.team_reply.text);
    return true;
}

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
//
// This is deliberately a presentation boundary.  It consumes locations only
// when the caller has already classified them as local or server-authorized
// teammate data.  It never accepts a client registry/entity view, which keeps
// replicated enemy transforms from becoming a HUD data source by accident.
constexpr int kRadarSlots = 10;

struct RadarBlip { bool visible = false; std::string x = "50.0%", y = "50.0%"; };

// Content owns this calibration.  rotation_degrees rotates world X/Z into the
// north-up image coordinate space around origin_x/origin_z before scaling.
struct RadarMapTransform {
    bool configured = false;
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    float world_width = 0.0f;
    float world_height = 0.0f;
    float rotation_degrees = 0.0f;
};

inline bool isValidRadarMapTransform(const RadarMapTransform& map) {
    return map.configured && std::isfinite(map.origin_x) && std::isfinite(map.origin_z) &&
        std::isfinite(map.world_width) && std::isfinite(map.world_height) &&
        std::isfinite(map.rotation_degrees) && map.world_width > 0.0f && map.world_height > 0.0f;
}

inline RadarBlip projectRadarBlip(float wx, float wz, const RadarMapTransform& map, bool alive) {
    RadarBlip out;
    if (!alive || !isValidRadarMapTransform(map) || !std::isfinite(wx) || !std::isfinite(wz))
        return out;

    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    const float radians = map.rotation_degrees * kDegreesToRadians;
    const float local_x = wx - map.origin_x;
    const float local_z = wz - map.origin_z;
    const float map_x = std::cos(radians) * local_x - std::sin(radians) * local_z;
    const float map_z = std::sin(radians) * local_x + std::cos(radians) * local_z;
    // `rotation_degrees` follows the content convention: positive values turn
    // world X toward map-down/world Z.  The 90 degree calibration therefore
    // maps world +X to the bottom edge of a north-up image.  Trigonometric
    // cardinal rotations can land a few ulps outside an edge (for example,
    // cos(90 degrees) * width).  Normalize only that numerical residue before
    // applying the deliberate off-map rejection.
    constexpr float kRadarBoundsEpsilon = 0.0001f;
    float u = map_x / map.world_width;
    float v = map_z / map.world_height;
    if (u >= -kRadarBoundsEpsilon && u <= 1.0f + kRadarBoundsEpsilon)
        u = std::clamp(u, 0.0f, 1.0f);
    if (v >= -kRadarBoundsEpsilon && v <= 1.0f + kRadarBoundsEpsilon)
        v = std::clamp(v, 0.0f, 1.0f);
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return out; // off-map hides rather than clamping to a misleading edge.

    char xb[16], yb[16];
    std::snprintf(xb, sizeof(xb), "%.1f%%", u * 100.0f);
    std::snprintf(yb, sizeof(yb), "%.1f%%", v * 100.0f);
    out.visible = true; out.x = xb; out.y = yb;
    return out;
}

enum class RadarLocationKind : uint8_t {
    LocalSelf,
    ApprovedTeammate,
    Bomb,
    Enemy,
};

// Network/custom-message code must create this value after applying its own
// audience rules.  Raw replicated ECS entities intentionally cannot satisfy
// this interface.
struct RadarApprovedLocation {
    uint32_t stable_id = 0;
    RadarLocationKind kind = RadarLocationKind::Enemy;
    FpsTeam team = FpsTeam::None;
    bool alive = false;
    float world_x = 0.0f;
    float world_z = 0.0f;
};

struct RadarPresentationSource {
    bool connected = false;
    const ClientCompetitiveState* competitive = nullptr;
    RadarMapTransform map;
    uint32_t local_stable_id = 0;

    // False until a server-owned team-location publication path exists.  The
    // existing protocol has no such message, so the template currently sets
    // this false and contributes only the local self location.
    bool has_authoritative_team_locations = false;
    std::vector<RadarApprovedLocation> approved_locations;
};

struct RadarSlotState {
    uint16_t round_number = 0;
    bool initialized = false;
    std::array<uint32_t, kRadarSlots> stable_ids{};

    void reset() {
        round_number = 0;
        initialized = false;
        stable_ids.fill(0);
    }
};

struct RadarPresentation {
    bool visible = false;
    bool neutral_visible = false;
    std::string neutral_text = "RADAR UNAVAILABLE";
    std::array<RadarBlip, kRadarSlots> blips{};
};

inline bool isApprovedRadarLocation(const RadarApprovedLocation& location,
                                    const RadarPresentationSource& source) {
    const ClientCompetitiveState* competitive = source.competitive;
    if (!competitive || location.stable_id == 0)
        return false;

    switch (location.kind) {
    case RadarLocationKind::LocalSelf:
        return location.stable_id == source.local_stable_id;
    case RadarLocationKind::ApprovedTeammate:
        return source.has_authoritative_team_locations &&
            Competitive::isPlayingTeam(competitive->local_team) &&
            location.team == competitive->local_team &&
            location.stable_id != source.local_stable_id;
    case RadarLocationKind::Bomb:
    case RadarLocationKind::Enemy:
        // D10 keeps bomb status in the existing round HUD and never draws
        // enemy locations.  A future bomb-location message must add a new
        // reviewed presentation contract before it can appear here.
        return false;
    }
    return false;
}

inline RadarPresentation buildRadarPresentation(
    RadarSlotState& slots, const RadarPresentationSource& source) {
    RadarPresentation out;
    const ClientCompetitiveState* competitive = source.competitive;
    if (!source.connected || !competitive || !competitive->has_round_state)
        return out;

    if (!isValidRadarMapTransform(source.map)) {
        // Missing map calibration is a neutral content failure.  It leaves
        // every slot hidden and does not block the rest of the competitive HUD.
        out.neutral_visible = true;
        return out;
    }

    out.visible = true;
    if (!slots.initialized || slots.round_number != competitive->round_number) {
        slots.initialized = true;
        slots.round_number = competitive->round_number;
        slots.stable_ids.fill(0);
    }

    // Slot zero is permanently reserved for the local marker.  Teammate slots
    // retain their stable id for the round, so another player's death or leave
    // cannot make surviving blips change identity.
    for (const RadarApprovedLocation& location : source.approved_locations) {
        if (!isApprovedRadarLocation(location, source))
            continue;

        int slot = -1;
        if (location.kind == RadarLocationKind::LocalSelf) {
            slot = 0;
            slots.stable_ids[0] = location.stable_id;
        } else {
            for (int i = 1; i < kRadarSlots; ++i) {
                if (slots.stable_ids[i] == location.stable_id) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                for (int i = 1; i < kRadarSlots; ++i) {
                    if (slots.stable_ids[i] == 0) {
                        slots.stable_ids[i] = location.stable_id;
                        slot = i;
                        break;
                    }
                }
            }
        }
        if (slot >= 0)
            out.blips[slot] = projectRadarBlip(
                location.world_x, location.world_z, source.map, location.alive);
    }
    return out;
}

struct RadarThrottle {
    float accumulator = 1.0f; // first update immediate
    bool shouldUpdate(float dt) {
        accumulator += dt;
        if (accumulator >= 0.1f) { accumulator = 0.0f; return true; } // 10 Hz
        return false;
    }
    void reset() { accumulator = 1.0f; }
};

} // namespace UiLogic
