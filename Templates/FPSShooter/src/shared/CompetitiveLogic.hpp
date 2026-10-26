#pragma once
// FPSShooter competitive (round-based) mode: game layer only.
// All [P] numbers live in ModeConfig/EconomyConfig data below (spec S1-S4),
// never as constants in logic. Pure logic so Tests/GameplayTests can drive
// it without a server; the server wires it to ECS + custom messages.
#include "SharedComponents.hpp"
#include "WeaponTypes.hpp"
#include "FpsGameLogic.hpp" // engine Team/Inventory/Wallet/Timer primitives + loadout helpers
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Competitive {

// --- Teams (on engine TeamComponent team_id) --------------------------------
enum class FpsTeam : uint8_t { None = 0, Attackers = 1, Defenders = 2, Spectator = 3 };
inline uint8_t teamId(FpsTeam t) { return static_cast<uint8_t>(t); }
inline FpsTeam teamFromId(uint8_t id) {
    return (id == 1) ? FpsTeam::Attackers : (id == 2) ? FpsTeam::Defenders
         : (id == 3) ? FpsTeam::Spectator : FpsTeam::None;
}
inline bool isPlayingTeam(FpsTeam t) { return t == FpsTeam::Attackers || t == FpsTeam::Defenders; }
inline FpsTeam otherPlayingTeam(FpsTeam t) {
    return t == FpsTeam::Attackers ? FpsTeam::Defenders : FpsTeam::Attackers;
}
// Auto-assign: least-populated playing team, tie -> Defenders (spec S2 [P]).
inline FpsTeam autoAssignTeam(int32_t attackers, int32_t defenders) {
    return attackers < defenders ? FpsTeam::Attackers : FpsTeam::Defenders;
}

// --- Config data --------------------------------------------------------------
struct ModeConfig {
    const char* id = "competitive";
    bool round_based = true;
    bool economy_enabled = true;
    bool halftime_enabled = true;
    bool overtime_enabled = true;
    int32_t max_rounds = 24;          // [P] spec S1.2
    int32_t overtime_max_rounds = 6;  // [P]
    float warmup_seconds = 30.0f;     // [V]
    float freeze_seconds = 6.0f;      // [V]
    float round_seconds = 115.0f;     // [P] 1:55
    float round_end_seconds = 7.0f;   // [V]
    float halftime_seconds = 15.0f;   // [V]
    float match_end_seconds = 15.0f;  // [V]
    float buy_window_after_live = 20.0f; // [P]
    float buy_zone_radius = 12.0f;    // distance-to-spawn buy zone fallback
    int32_t start_money = 800;        // [P]
    int32_t overtime_start_money = 10000; // [P]
    int32_t money_cap = 16000;        // [P]
    int32_t team_size = 5;            // [P] 5v5
    float tick_rate = 60.0f;
    int32_t winsToClinch() const { return max_rounds / 2 + 1; }              // 13
    int32_t overtimeWinsToClinch() const { return overtime_max_rounds / 2 + 1; } // 4
    int32_t currentStartMoney(bool overtime) const { return overtime ? overtime_start_money : start_money; }
};

struct EconomyConfig {
    int32_t win_elimination = 3250, win_bomb = 3500, win_defuse = 3500, win_time = 3250; // [P]
    int32_t loss_base = 1400, loss_step = 500, loss_cap_streak = 5; // 1400..3400
    int32_t plant_planter = 300, plant_team = 800, defuse_bonus = 300, suicide_penalty = -300; // [P]
};

// --- Round FSM ------------------------------------------------------------------
enum class RoundPhase : uint8_t {
    WaitingForPlayers = 0, Warmup = 1, Freeze = 2, FreezeTime = 2, Live = 3,
    RoundEnd = 4, Halftime = 5, MatchEnded = 6
};
enum class RoundEndReason : uint8_t {
    None = 0, TargetBombed = 1, BombDefused = 2, AttackersElimination = 3,
    DefendersElimination = 4, RoundDraw = 5, TargetSaved = 6, HostagesNotRescued = 7
};
enum class RoundWinner : uint8_t { None = 0, Draw = 1, Attackers = 2, Defenders = 3 };
inline RoundWinner winnerOfTeam(FpsTeam t) {
    return t == FpsTeam::Attackers ? RoundWinner::Attackers
         : t == FpsTeam::Defenders ? RoundWinner::Defenders : RoundWinner::None;
}

// Singleton match state (one entity on the server). phase_time_remaining and
// phase_deadline_tick (server tick) are replicated via ROUND_STATE.
struct MatchStateComponent {
    RoundPhase phase = RoundPhase::WaitingForPlayers;
    float phase_time_remaining = 0.0f;
    uint32_t phase_deadline_tick = 0;
    uint32_t server_tick = 0;
    uint16_t round_number = 0;
    uint8_t wins_attackers = 0;
    uint8_t wins_defenders = 0;
    uint8_t ot_wins_attackers = 0;
    uint8_t ot_wins_defenders = 0;
    int32_t first_half_wins_attackers = 0;
    int32_t first_half_wins_defenders = 0;
    bool bomb_planted = false;
    bool overtime = false;
    bool paused = false;
    bool teams_swapped = false; // current side assignment flipped vs match start
    RoundEndReason last_reason = RoundEndReason::None;
    RoundWinner last_winner = RoundWinner::None;
    RoundWinner match_winner = RoundWinner::None;
};

// Bomb hooks: interface only in this stage (plant/defuse land next stage).
struct IBombHooks {
    virtual ~IBombHooks() = default;
    virtual bool isPlanted() const { return false; }
    virtual bool consumeDetonated() { return false; }
    virtual bool consumeDefused() { return false; }
};
struct NullBombHooks : IBombHooks {};

struct RoundTickInput {
    int32_t attackers_alive = 0;
    int32_t defenders_alive = 0;
    int32_t attackers_connected = 0;
    int32_t defenders_connected = 0;
    bool bomb_planted = false;
    bool bomb_detonated = false;
    bool bomb_defused = false;
    bool hasPlayingPlayers() const { return attackers_connected + defenders_connected > 0; }
};

class RoundSystem {
public:
    ModeConfig config;
    MatchStateComponent state;
    CountdownTimerComponent timer; // engine primitive driving phase durations
    const IBombHooks* bomb_hooks = nullptr;
    // Server-side effects (respawn on Freeze, team/money swap on Halftime...).
    std::function<void(RoundPhase)> on_phase_enter;
    std::function<void(RoundWinner, RoundEndReason)> on_round_end;
    std::function<void(bool overtime_period)> on_period_reset; // halftime / OT half: swap + money reset
    bool period_reset_pending = false;
    bool period_reset_overtime = false;

    void configure(const ModeConfig& cfg) { config = cfg; resetMatch(); }
    void resetMatch() {
        state = MatchStateComponent{};
        reg_rounds_played_ = 0; ot_rounds_played_ = 0;
        halftime_done_ = false; ot_halftime_done_ = false;
        phase_elapsed_ = 0.0f;
        enterPhase(RoundPhase::WaitingForPlayers);
    }
    float phaseElapsed() const { return phase_elapsed_; }
    int32_t roundsPlayed() const { return reg_rounds_played_ + ot_rounds_played_; }

    static bool isLegalTransition(RoundPhase from, RoundPhase to) {
        using P = RoundPhase;
        if (from == to) return false;
        switch (from) {
        case P::WaitingForPlayers: return to == P::Warmup || to == P::Freeze;
        case P::Warmup: return to == P::Freeze;
        case P::Freeze: return to == P::Live;
        case P::Live: return to == P::RoundEnd;
        case P::RoundEnd: return to == P::Freeze || to == P::Halftime || to == P::MatchEnded;
        case P::Halftime: return to == P::Freeze;
        case P::MatchEnded: return to == P::WaitingForPlayers || to == P::Warmup;
        }
        return false;
    }
    bool transitionTo(RoundPhase to) {
        if (!isLegalTransition(state.phase, to)) return false; // illegal: rejected
        enterPhase(to);
        return true;
    }
    void setPaused(bool p) { state.paused = p; } // mp_pause_match: holds Freeze indefinitely

    bool isBuyPhaseOpen(float seconds_into_live) const {
        if (state.phase == RoundPhase::Warmup || state.phase == RoundPhase::Freeze) return true;
        if (state.phase == RoundPhase::Live) return seconds_into_live <= config.buy_window_after_live;
        return false;
    }
    bool isMovementLocked() const { return state.phase == RoundPhase::Freeze; }

    void tick(float dt, const RoundTickInput& in) {
        if (dt <= 0.0f || state.paused) return;
        state.server_tick += static_cast<uint32_t>(std::max(1.0f, dt * config.tick_rate));
        phase_elapsed_ += dt;
        RoundTickInput input = in;
        if (bomb_hooks) {
            input.bomb_planted = input.bomb_planted || bomb_hooks->isPlanted();
            // consume* are non-const on the interface; server owns a mutable hook.
            auto* hooks = const_cast<IBombHooks*>(bomb_hooks);
            input.bomb_detonated = input.bomb_detonated || hooks->consumeDetonated();
            input.bomb_defused = input.bomb_defused || hooks->consumeDefused();
        }
        state.bomb_planted = input.bomb_planted;

        switch (state.phase) {
        case RoundPhase::WaitingForPlayers:
            if (input.hasPlayingPlayers())
                transitionTo(config.warmup_seconds > 0.0f ? RoundPhase::Warmup : RoundPhase::Freeze);
            break;
        case RoundPhase::Live: {
            // Win checks every Live tick, spec S9 order.
            if (input.bomb_detonated) { endRound(RoundWinner::Attackers, RoundEndReason::TargetBombed); break; }
            if (input.bomb_defused) { endRound(RoundWinner::Defenders, RoundEndReason::BombDefused); break; }
            if (input.defenders_alive <= 0 && input.defenders_connected > 0) {
                endRound(RoundWinner::Attackers, RoundEndReason::AttackersElimination); break;
            }
            if (input.attackers_alive <= 0 && input.attackers_connected > 0 && !input.bomb_planted) {
                endRound(RoundWinner::Defenders, RoundEndReason::DefendersElimination); break;
            }
            if (timer.tick(dt)) {
                if (input.bomb_planted) {
                    timer.start(config.round_seconds); // planted: round timer no longer ends round
                } else {
                    endRound(RoundWinner::Defenders, RoundEndReason::TargetSaved);
                }
            }
            break;
        }
        case RoundPhase::Warmup:
        case RoundPhase::Freeze:
        case RoundPhase::RoundEnd:
        case RoundPhase::Halftime:
        case RoundPhase::MatchEnded:
            if (timer.tick(dt)) advanceAfterTimer();
            break;
        }
        state.phase_time_remaining = timer.remaining;
        state.phase_deadline_tick = state.server_tick +
            static_cast<uint32_t>(timer.remaining * config.tick_rate);
    }

    // End the current Live round (also used by tests / future bomb system).
    void endRound(RoundWinner winner, RoundEndReason reason) {
        if (state.phase != RoundPhase::Live) return;
        state.last_winner = winner;
        state.last_reason = reason;
        if (winner == RoundWinner::Attackers) {
            state.wins_attackers++;
            if (state.overtime) state.ot_wins_attackers++;
        } else if (winner == RoundWinner::Defenders) {
            state.wins_defenders++;
            if (state.overtime) state.ot_wins_defenders++;
        }
        if (state.overtime) ot_rounds_played_++; else reg_rounds_played_++;
        enterPhase(RoundPhase::RoundEnd);
        if (on_round_end) on_round_end(winner, reason);
    }

private:
    int32_t reg_rounds_played_ = 0;
    int32_t ot_rounds_played_ = 0;
    bool halftime_done_ = false;
    bool ot_halftime_done_ = false;
    float phase_elapsed_ = 0.0f;

    float durationFor(RoundPhase p) const {
        switch (p) {
        case RoundPhase::Warmup: return config.warmup_seconds;
        case RoundPhase::Freeze: return config.freeze_seconds;
        case RoundPhase::Live: return config.round_seconds;
        case RoundPhase::RoundEnd: return config.round_end_seconds;
        case RoundPhase::Halftime: return config.halftime_seconds;
        case RoundPhase::MatchEnded: return config.match_end_seconds;
        default: return 0.0f;
        }
    }
    void enterPhase(RoundPhase p) {
        state.phase = p;
        phase_elapsed_ = 0.0f;
        float d = durationFor(p);
        if (d > 0.0f) timer.start(d); else timer.stop();
        state.phase_time_remaining = timer.remaining;
        state.phase_deadline_tick = state.server_tick +
            static_cast<uint32_t>(timer.remaining * config.tick_rate);
        if (p == RoundPhase::Freeze) state.round_number = static_cast<uint16_t>(roundsPlayed() + 1);
        if (p == RoundPhase::Halftime) {
            if (!state.overtime) {
                state.first_half_wins_attackers = state.wins_attackers;
                state.first_half_wins_defenders = state.wins_defenders;
            }
            state.teams_swapped = !state.teams_swapped;
            period_reset_pending = true;
            period_reset_overtime = state.overtime;
            if (on_period_reset) on_period_reset(state.overtime);
        }
        if (on_phase_enter) on_phase_enter(p);
    }
    bool matchDecided(RoundWinner& out) const {
        if (state.overtime) {
            if (state.ot_wins_attackers >= config.overtimeWinsToClinch()) { out = RoundWinner::Attackers; return true; }
            if (state.ot_wins_defenders >= config.overtimeWinsToClinch()) { out = RoundWinner::Defenders; return true; }
            if (ot_rounds_played_ >= config.overtime_max_rounds) {
                out = state.ot_wins_attackers > state.ot_wins_defenders ? RoundWinner::Attackers
                    : state.ot_wins_defenders > state.ot_wins_attackers ? RoundWinner::Defenders
                    : RoundWinner::Draw;
                return true;
            }
            return false;
        }
        if (state.wins_attackers >= config.winsToClinch()) { out = RoundWinner::Attackers; return true; }
        if (state.wins_defenders >= config.winsToClinch()) { out = RoundWinner::Defenders; return true; }
        if (reg_rounds_played_ >= config.max_rounds) {
            out = state.wins_attackers > state.wins_defenders ? RoundWinner::Attackers
                : state.wins_defenders > state.wins_attackers ? RoundWinner::Defenders
                : RoundWinner::Draw;
            return true;
        }
        return false;
    }
    void advanceAfterTimer() {
        switch (state.phase) {
        case RoundPhase::Warmup: transitionTo(RoundPhase::Freeze); break;
        case RoundPhase::Freeze: transitionTo(RoundPhase::Live); break;
        case RoundPhase::Halftime: transitionTo(RoundPhase::Freeze); break;
        case RoundPhase::MatchEnded: resetMatch(); break;
        case RoundPhase::RoundEnd: {
            RoundWinner decided = RoundWinner::None;
            bool tiedRegulation = !state.overtime && reg_rounds_played_ >= config.max_rounds &&
                state.wins_attackers == state.wins_defenders;
            if (tiedRegulation && config.overtime_enabled) {
                // Start overtime instead of ending: OT period, money-reset hook.
                state.overtime = true;
                state.ot_wins_attackers = 0; state.ot_wins_defenders = 0;
                ot_rounds_played_ = 0; ot_halftime_done_ = false;
                period_reset_pending = true; period_reset_overtime = true;
                if (on_period_reset) on_period_reset(true);
                transitionTo(RoundPhase::Freeze);
                break;
            }
            if (matchDecided(decided)) {
                state.match_winner = decided;
                transitionTo(RoundPhase::MatchEnded);
                break;
            }
            if (!state.overtime && config.halftime_enabled && !halftime_done_ &&
                reg_rounds_played_ == config.max_rounds / 2) {
                halftime_done_ = true;
                transitionTo(RoundPhase::Halftime);
                break;
            }
            if (state.overtime && !ot_halftime_done_ &&
                ot_rounds_played_ == config.overtime_max_rounds / 2) {
                ot_halftime_done_ = true;
                transitionTo(RoundPhase::Halftime);
                break;
            }
            transitionTo(RoundPhase::Freeze);
            break;
        }
        default: break;
        }
    }
};

// --- Economy (money = engine ResourceWalletComponent balance) -------------------
struct MoneyComponent {
    ResourceWalletComponent wallet; // balance = money, cap = money_cap
    uint8_t loss_streak = 0;
    int32_t money() const { return static_cast<int32_t>(wallet.balance); }
    void reset(int32_t start, int32_t cap) {
        wallet = ResourceWalletComponent{};
        wallet.setCap(cap);
        wallet.setBalance(start);
        loss_streak = 0;
    }
};

struct EconomySystem {
    EconomyConfig config;
    // Award at RoundEnd. Returns amount granted to this player.
    int32_t awardRoundEnd(MoneyComponent& m, FpsTeam player_team, RoundWinner winner, RoundEndReason reason) const {
        if (!isPlayingTeam(player_team)) return 0;
        bool won = winnerOfTeam(player_team) == winner;
        int32_t amount = 0;
        if (winner == RoundWinner::Draw) {
            amount = config.loss_base;
        } else if (won) {
            switch (reason) {
            case RoundEndReason::TargetBombed: amount = config.win_bomb; break;
            case RoundEndReason::BombDefused: amount = config.win_defuse; break;
            case RoundEndReason::TargetSaved: amount = config.win_time; break;
            default: amount = config.win_elimination; break;
            }
            // Win decays the loss streak by 1, never below 1 once started, 0 stays 0.
            if (m.loss_streak > 1) m.loss_streak = static_cast<uint8_t>(m.loss_streak - 1);
        } else {
            int32_t streak = std::min<int32_t>(m.loss_streak, config.loss_cap_streak - 1);
            amount = config.loss_base + config.loss_step * streak; // 1400..3400
            if (m.loss_streak < config.loss_cap_streak) m.loss_streak++;
        }
        return static_cast<int32_t>(m.wallet.add(amount));
    }
    int32_t awardKill(MoneyComponent& m, const WeaponCatalogDef* def) const {
        return def ? static_cast<int32_t>(m.wallet.add(def->kill_award)) : 0;
    }
    // Bomb economy hooks (called by the future bomb system).
    int32_t awardPlantPlanter(MoneyComponent& m) const { return static_cast<int32_t>(m.wallet.add(config.plant_planter)); }
    int32_t awardPlantTeam(MoneyComponent& m) const { return static_cast<int32_t>(m.wallet.add(config.plant_team)); }
    int32_t awardDefuse(MoneyComponent& m) const { return static_cast<int32_t>(m.wallet.add(config.defuse_bonus)); }
    int32_t applySuicidePenalty(MoneyComponent& m) const {
        return -static_cast<int32_t>(m.wallet.remove(-config.suicide_penalty));
    }
};

// --- Teams: selection validation --------------------------------------------------
enum class TeamSelectResult : uint8_t { Ok = 0, TeamFull = 1, BothFull = 2, PhaseClosed = 3, InvalidTeam = 4 };
inline TeamSelectResult validateTeamSelect(FpsTeam desired, bool auto_assign,
                                           int32_t attackers, int32_t defenders,
                                           RoundPhase phase, const ModeConfig& cfg,
                                           FpsTeam& out_team) {
    out_team = FpsTeam::None;
    // Switch/join only in Warmup/Freeze (plus lobby/waiting) [P].
    if (!(phase == RoundPhase::WaitingForPlayers || phase == RoundPhase::Warmup || phase == RoundPhase::Freeze))
        return TeamSelectResult::PhaseClosed;
    if (!auto_assign && desired == FpsTeam::Spectator) { out_team = desired; return TeamSelectResult::Ok; }
    FpsTeam target = auto_assign ? autoAssignTeam(attackers, defenders) : desired;
    if (!isPlayingTeam(target)) return TeamSelectResult::InvalidTeam;
    int32_t count = target == FpsTeam::Attackers ? attackers : defenders;
    if (count >= cfg.team_size) {
        bool otherFull = (target == FpsTeam::Attackers ? defenders : attackers) >= cfg.team_size;
        return otherFull ? TeamSelectResult::BothFull : TeamSelectResult::TeamFull;
    }
    out_team = target;
    return TeamSelectResult::Ok;
}
// Late joiner spawns next Freeze when joining during Live/RoundEnd.
inline bool canSpawnNow(RoundPhase phase) {
    return phase == RoundPhase::WaitingForPlayers || phase == RoundPhase::Warmup || phase == RoundPhase::Freeze;
}

// Buy-zone check: distance-to-team-spawn fallback (chosen over trigger-volume
// wiring in this stage; TriggerComponent zones can replace this predicate
// without changing BuySystem, which takes in_buy_zone as an input).
inline bool isInBuyZone(const glm::vec3& player_pos, const std::vector<glm::vec3>& team_spawns, float radius) {
    for (const auto& s : team_spawns) if (glm::distance(player_pos, s) <= radius) return true;
    return false;
}

// --- Buy ----------------------------------------------------------------------------
enum class BuyResult : uint8_t {
    Ok = 0, UnknownItem = 1, InsufficientFunds = 2, WrongTeam = 3, OutsideBuyZone = 4,
    BuyTimeExpired = 5, Dead = 6, SlotOccupied = 7, AlreadyOwned = 8, PhaseClosed = 9,
    DuplicateRequest = 10, CarryLimit = 11
};

struct BuyContext {
    uint16_t client_id = 0;
    uint32_t request_seq = 0;
    RoundPhase phase = RoundPhase::Freeze;
    float seconds_into_live = 0.0f;
    bool alive = true;
    bool in_buy_zone = true;
    uint8_t team_id = 0; // engine TeamComponent value (1/2 playing)
    MoneyComponent* money = nullptr;
    InventoryComponent* inventory = nullptr;
    PlayerLoadoutComponent* loadout = nullptr;
    WeaponComponent* weapon = nullptr;
    ArmorComponent* armor = nullptr;
};

class BuySystem {
public:
    ModeConfig config;
    // Execute one buy. DroppedItem (replaced primary/secondary) returned via
    // out_dropped. Exactly-once: a repeated request_seq returns the cached
    // result as DuplicateRequest with no mutation.
    BuyResult buy(const BuyContext& ctx, uint16_t item_id, FpsLogic::DroppedItem* out_dropped = nullptr) {
        // COUNT sentinel: caller spawns out_dropped only when type != COUNT.
        if (out_dropped) *out_dropped = FpsLogic::DroppedItem{WeaponType::COUNT, 0, 0};
        // Dedup first: replayed seq never re-charges / re-gives.
        auto seen = last_seq_.find(ctx.client_id);
        if (ctx.request_seq != 0 && seen != last_seq_.end() && ctx.request_seq <= seen->second)
            return BuyResult::DuplicateRequest;
        BuyResult result = buyImpl(ctx, item_id, out_dropped);
        if (ctx.request_seq != 0) {
            last_seq_[ctx.client_id] = ctx.request_seq;
            last_result_[ctx.client_id] = result;
        }
        return result;
    }
    BuyResult lastResultFor(uint16_t client_id) const {
        auto it = last_result_.find(client_id);
        return it == last_result_.end() ? BuyResult::UnknownItem : it->second;
    }

private:
    std::unordered_map<uint16_t, uint32_t> last_seq_;
    std::unordered_map<uint16_t, BuyResult> last_result_;

    BuyResult buyImpl(const BuyContext& ctx, uint16_t item_id, FpsLogic::DroppedItem* out_dropped) {
        const WeaponCatalogDef* def = findCatalogById(item_id);
        if (!def) return BuyResult::UnknownItem;
        if (!ctx.alive) return BuyResult::Dead;
        // Phase / window.
        if (ctx.phase == RoundPhase::Warmup || ctx.phase == RoundPhase::Freeze) {
            // open
        } else if (ctx.phase == RoundPhase::Live) {
            if (ctx.seconds_into_live > config.buy_window_after_live) return BuyResult::BuyTimeExpired;
        } else {
            return BuyResult::PhaseClosed;
        }
        if (!ctx.in_buy_zone) return BuyResult::OutsideBuyZone;
        uint8_t team_bit = ctx.team_id == teamId(FpsTeam::Attackers) ? kCatalogTeamAttackers
                         : ctx.team_id == teamId(FpsTeam::Defenders) ? kCatalogTeamDefenders : 0;
        if (team_bit == 0 || (def->team_mask & team_bit) == 0) return BuyResult::WrongTeam;
        if (!ctx.money || !ctx.inventory || !ctx.loadout || !ctx.weapon) return BuyResult::UnknownItem;

        // Slot / ownership pre-checks (no mutation yet).
        int slot = static_cast<int>(def->slot);
        if (def->kind == BuyItemKind::Grenade) {
            if (ctx.loadout->slots[slot].occupied) return BuyResult::CarryLimit;
        }
        if (def->kind == BuyItemKind::Equipment) {
            if (!ctx.armor) return BuyResult::UnknownItem;
            if (def->id == 42 && ctx.armor->armor >= 100) return BuyResult::AlreadyOwned;
            if (def->id == 43 && ctx.armor->armor >= 100 && ctx.armor->has_helmet) return BuyResult::AlreadyOwned;
            if (def->id == 44 && ctx.armor->has_defuse_kit) return BuyResult::AlreadyOwned;
        }
        // Funds: wallet trySpend is atomic (no mutation on failure).
        if (!ctx.money->wallet.trySpend(def->price)) return BuyResult::InsufficientFunds;

        // Give (all-or-nothing past this point).
        if (def->kind == BuyItemKind::Equipment) {
            if (def->id == 42) ctx.armor->armor = 100;
            else if (def->id == 43) { ctx.armor->armor = 100; ctx.armor->has_helmet = true; }
            else if (def->id == 44) ctx.armor->has_defuse_kit = true;
            return BuyResult::Ok;
        }
        // Weapon / grenade into its slot; an occupied primary/secondary is
        // replaced (old weapon reported as dropped), per spec S4.5.
        auto& target = ctx.loadout->slots[slot];
        if (target.occupied && out_dropped) {
            *out_dropped = FpsLogic::DroppedItem{target.type, target.ammo, target.reserve};
        }
        bool was_active = ctx.loadout->active_slot == slot;
        target = WeaponSlotState{def->weapon_type, def->mag, def->reserve, true};
        if (ctx.loadout->active_slot < 0) ctx.loadout->active_slot = slot;
        if (was_active) FpsLogic::loadSlotIntoWeapon(*ctx.loadout, *ctx.weapon, slot);
        FpsLogic::syncInventory(*ctx.inventory, *ctx.loadout);
        return BuyResult::Ok;
    }
};

} // namespace Competitive
