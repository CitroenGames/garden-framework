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
#include <limits>
#include <string>
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
    float buy_zone_radius = 12.0f;    // sphere trigger radius used at team spawns when no authored zones exist
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
    // Existing server fallback for deaths without a catalog weapon definition.
    int32_t uncatalogued_kill = 300;
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

// --- C4 bomb (Stage 3.4, damage_objectives.md section 3) ----------------------
// Pure state machine; the server supplies site geometry, positions and the
// per-tick channel inputs derived from real input/health/weapon state.
enum class BombState : uint8_t { Idle = 0, Carried = 1, Dropped = 2, Planting = 3, Planted = 4, Defusing = 5, Exploded = 6, Defused = 7 };

struct BombSite {
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
    uint8_t site_index = 0;
};

struct BombConfig {
    float plant_seconds = 3.0f;          // [V]
    float defuse_with_kit_seconds = 5.0f;    // [V]
    float defuse_without_kit_seconds = 10.0f; // [V]
    float bomb_timer_seconds = 40.0f;    // [V]
    float pickup_radius = 1.5f;
    float defuse_range = 3.0f;
    float move_speed_threshold = 0.5f; // server-side movement cancel input
    // Documented fallback when the level exposes no bomb-site volumes:
    // sites are derived from Defender spawn positions (centre + this radius).
    float fallback_site_radius = 12.0f;
    std::vector<BombSite> sites;
    bool inSite(const glm::vec3& pos, uint8_t& out_site) const {
        for (const auto& s : sites) if (glm::distance(pos, s.centre) <= s.radius) { out_site = s.site_index; return true; }
        return false;
    }
};

// Authored bomb sites for a shipped level (assets/levels/main.sites.json).
// Format: {"sites":[{"name":"A","centre":[x,y,z],"radius":r}, ...]}
// Returns false (out cleared) on any malformed/insufficient data so the
// caller can fall back to the Defender-spawn fallback above.
inline bool parseAuthoredBombSites(const std::string& text, std::vector<BombSite>& out, std::string* err = nullptr)
{
    out.clear();
    auto failWith = [&](const char* m) { if (err) *err = m; out.clear(); return false; };
    size_t pos = text.find("\"sites\"");
    if (pos == std::string::npos) return failWith("no sites key");
    pos = text.find('[', pos);
    if (pos == std::string::npos) return failWith("sites not an array");
    while (true) {
        size_t obj = text.find('{', pos);
        if (obj == std::string::npos) break;
        size_t end = text.find('}', obj);
        if (end == std::string::npos) return failWith("unterminated site object");
        std::string chunk = text.substr(obj, end - obj);
        size_t cpos = chunk.find("\"centre\"");
        if (cpos == std::string::npos) return failWith("site missing centre");
        cpos = chunk.find('[', cpos);
        size_t cend = chunk.find(']', cpos);
        if (cpos == std::string::npos || cend == std::string::npos) return failWith("centre not an array");
        std::string nums = chunk.substr(cpos + 1, cend - cpos - 1);
        float v[3] = {0, 0, 0}; int n = 0; size_t s = 0;
        while (n < 3) { size_t comma = nums.find(',', s); std::string tok = nums.substr(s, comma == std::string::npos ? std::string::npos : comma - s); try { v[n] = std::stof(tok); } catch (...) { return failWith("centre not numeric"); } ++n; if (comma == std::string::npos) break; s = comma + 1; }
        if (n != 3) return failWith("centre needs 3 numbers");
        size_t rpos = chunk.find("\"radius\"");
        if (rpos == std::string::npos) return failWith("site missing radius");
        rpos = chunk.find(':', rpos);
        float radius = 0.0f;
        try { radius = std::stof(chunk.substr(rpos + 1)); } catch (...) { return failWith("radius not numeric"); }
        if (radius < 3.0f || radius > 30.0f) return failWith("radius out of [3,30]");
        BombSite site; site.centre = glm::vec3(v[0], v[1], v[2]); site.radius = radius; site.site_index = (uint8_t)out.size();
        out.push_back(site);
        pos = end + 1;
        if (text.find('{', pos) == std::string::npos || (out.size() >= 1 && text.find(']', pos) < text.find('{', pos))) break;
    }
    if (out.size() < 2) return failWith("need at least 2 authored sites");
    for (size_t i = 0; i < out.size(); ++i) for (size_t j = i + 1; j < out.size(); ++j)
        if (glm::distance(out[i].centre, out[j].centre) < 5.0f) return failWith("authored sites too close (<5m)");
    return true;
}

// Singleton bomb state (mirrors BombStateComponent in the spec).
struct BombStateComponent {
    BombState state = BombState::Idle;
    entt::entity carrier = entt::null;
    entt::entity planter = entt::null;
    entt::entity defuser = entt::null;
    uint8_t site = 0;
    float plant_channel = 0.0f;
    float defuse_channel = 0.0f;
    float timer = 0.0f;
    glm::vec3 dropped_position{0.0f};
    glm::vec3 planted_position{0.0f};
};

struct BombTickInput {
    // Carrier/plant channel inputs.
    bool plant_use = false;
    bool plant_in_site = false;
    uint8_t plant_site = 0;
    bool plant_moved = false;
    bool plant_damaged = false;
    bool plant_weapon_switched = false;
    bool plant_dead = false;
    glm::vec3 carrier_position{0.0f};
    // Defuse channel inputs (defuse_actor identifies the would-be defuser).
    entt::entity defuse_actor = entt::null;
    bool defuse_use = false;
    bool defuse_in_range = false;
    bool defuse_has_kit = false;
    bool defuse_moved = false;
    bool defuse_damaged = false;
    bool defuse_dead = false;
    bool paused = false;
};

class BombSystem {
public:
    BombConfig config;
    BombStateComponent bomb;
    std::function<void(BombState from, BombState to)> on_transition;

    bool isPlanted() const { return bomb.state == BombState::Planted || bomb.state == BombState::Defusing; }
    bool consumeDetonated() { bool v = detonated_pending_; detonated_pending_ = false; return v; }
    bool consumeDefused() { bool v = defused_pending_; defused_pending_ = false; return v; }
    bool consumePlantCompleted(entt::entity& out_planter) {
        if (!plant_pending_) return false; plant_pending_ = false; out_planter = bomb.planter; return true;
    }
    bool consumeDefuseCompleted(entt::entity& out_defuser) {
        if (!defuse_reward_pending_) return false; defuse_reward_pending_ = false; out_defuser = last_defuser_; return true;
    }
    float activeDefuseDuration() const { return active_defuse_duration_; }

    void resetForRound() {
        BombState old = bomb.state;
        bomb = BombStateComponent{};
        detonated_pending_ = defused_pending_ = plant_pending_ = defuse_reward_pending_ = false;
        active_defuse_duration_ = config.defuse_without_kit_seconds;
        if (old != bomb.state && on_transition) on_transition(old, bomb.state);
    }
    bool assignCarrier(entt::entity carrier) {
        if (bomb.state != BombState::Idle || carrier == entt::null) return false;
        bomb.carrier = carrier;
        setState(BombState::Carried);
        return true;
    }
    // Carrier death/disconnect: bomb drops at the last carrier position.
    bool dropBomb(const glm::vec3& pos) {
        if (bomb.state != BombState::Carried && bomb.state != BombState::Planting) return false;
        bomb.carrier = entt::null; bomb.plant_channel = 0.0f; bomb.dropped_position = pos;
        setState(BombState::Dropped);
        return true;
    }
    bool onCarrierDeath(const glm::vec3& pos) { return dropBomb(pos); }
    bool onCarrierDisconnect(const glm::vec3& pos) { return dropBomb(pos); }
    // Attackers-only pickup of a dropped bomb.
    bool tryPickup(entt::entity actor, FpsTeam team, const glm::vec3& actor_pos) {
        if (bomb.state != BombState::Dropped || actor == entt::null) return false;
        if (team != FpsTeam::Attackers) return false;
        if (glm::distance(actor_pos, bomb.dropped_position) > config.pickup_radius) return false;
        bomb.carrier = actor;
        setState(BombState::Carried);
        return true;
    }

    void tick(float dt, const BombTickInput& in) {
        if (dt <= 0.0f || in.paused) return; // paused match: channels + timer frozen
        // Carrier death drops the bomb before any channel progress.
        if ((bomb.state == BombState::Carried || bomb.state == BombState::Planting) && in.plant_dead) {
            dropBomb(in.carrier_position);
            return;
        }
        if (bomb.state == BombState::Carried) {
            if (in.plant_use && in.plant_in_site && !in.plant_moved && !in.plant_damaged && !in.plant_weapon_switched) {
                bomb.site = in.plant_site; bomb.plant_channel = 0.0f;
                setState(BombState::Planting);
            } else return;
        }
        if (bomb.state == BombState::Planting) {
            if (!in.plant_use || !in.plant_in_site || in.plant_moved || in.plant_damaged || in.plant_weapon_switched) {
                bomb.plant_channel = 0.0f; // channel cancels, bomb stays carried
                setState(BombState::Carried);
                return;
            }
            bomb.site = in.plant_site;
            bomb.plant_channel += dt;
            if (bomb.plant_channel >= config.plant_seconds - 0.0001f) {
                bomb.planter = bomb.carrier; bomb.planted_position = in.carrier_position;
                bomb.timer = config.bomb_timer_seconds; bomb.plant_channel = 0.0f;
                plant_pending_ = true;
                setState(BombState::Planted);
            }
            return;
        }
        if (bomb.state == BombState::Planted || bomb.state == BombState::Defusing) {
            // Timer first: explosion wins a same-tick defuse tie, and a defuser
            // who dies on the completion tick is cancelled below before progress.
            bomb.timer -= dt;
            if (bomb.timer <= dt * 0.5f) { // half-tick tolerance: same-tick defuse tie explodes
                bomb.timer = 0.0f; bomb.defuser = entt::null; bomb.defuse_channel = 0.0f;
                detonated_pending_ = true;
                setState(BombState::Exploded);
                return;
            }
            if (bomb.state == BombState::Planted) {
                if (in.defuse_use && in.defuse_in_range && in.defuse_actor != entt::null && !in.defuse_dead) {
                    bomb.defuser = in.defuse_actor; bomb.defuse_channel = 0.0f;
                    active_defuse_duration_ = in.defuse_has_kit ? config.defuse_with_kit_seconds
                                                                : config.defuse_without_kit_seconds;
                    setState(BombState::Defusing);
                } else return;
            }
            if (bomb.state == BombState::Defusing) {
                if (in.defuse_actor != bomb.defuser) return; // single defuser: second USE ignored
                if (!in.defuse_use || !in.defuse_in_range || in.defuse_moved || in.defuse_damaged || in.defuse_dead) {
                    bomb.defuser = entt::null; bomb.defuse_channel = 0.0f;
                    setState(BombState::Planted);
                    return;
                }
                bomb.defuse_channel += dt;
                if (bomb.defuse_channel >= active_defuse_duration_ - 0.0001f) {
                    last_defuser_ = bomb.defuser; bomb.defuser = entt::null; bomb.defuse_channel = 0.0f;
                    defused_pending_ = true; defuse_reward_pending_ = true;
                    setState(BombState::Defused);
                }
            }
        }
    }

private:
    bool detonated_pending_ = false;
    bool defused_pending_ = false;
    bool plant_pending_ = false;
    bool defuse_reward_pending_ = false;
    entt::entity last_defuser_ = entt::null;
    float active_defuse_duration_ = 10.0f;
    void setState(BombState s) {
        if (bomb.state == s) return;
        BombState old = bomb.state; bomb.state = s;
        if (on_transition) on_transition(old, s);
    }
};

// Real IBombHooks adapter over a BombSystem (replaces NullBombHooks server-side).
struct BombHooksAdapter : IBombHooks {
    BombSystem* system = nullptr;
    explicit BombHooksAdapter(BombSystem* s = nullptr) : system(s) {}
    bool isPlanted() const override { return system && system->isPlanted(); }
    bool consumeDetonated() override { return system && system->consumeDetonated(); }
    bool consumeDefused() override { return system && system->consumeDefused(); }
};

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

// The result selected for a Live tick before its state is mutated.  Keeping
// this selection pure makes the simultaneous-event policy visible to both the
// server wiring and the deterministic gameplay tests.
struct RoundOutcome {
    RoundWinner winner = RoundWinner::None;
    RoundEndReason reason = RoundEndReason::None;

    bool resolved() const { return winner != RoundWinner::None; }
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

    // The authoritative Live-tick winner order is fixed deliberately:
    // detonation, defuse, Defender elimination, Attacker elimination, then
    // an unplanted phase-timer expiry.  Thus a same-tick detonate/defuse is a
    // detonation, simultaneous empty teams award Attackers by the existing
    // Defender-elimination rule, and a planted bomb suppresses Attacker
    // elimination and the round timer until the bomb itself resolves.
    //
    // `phase_timer_expired` is supplied only after the phase timer was ticked;
    // callers use false for events that occur before that timer advances.
    static RoundOutcome resolveLiveOutcome(const RoundTickInput& input,
                                           bool phase_timer_expired) {
        if (input.bomb_detonated)
            return {RoundWinner::Attackers, RoundEndReason::TargetBombed};
        if (input.bomb_defused)
            return {RoundWinner::Defenders, RoundEndReason::BombDefused};
        // Existing reason labels name the winning side's elimination outcome.
        if (input.defenders_alive <= 0 && input.defenders_connected > 0)
            return {RoundWinner::Attackers, RoundEndReason::AttackersElimination};
        if (input.attackers_alive <= 0 && input.attackers_connected > 0 && !input.bomb_planted)
            return {RoundWinner::Defenders, RoundEndReason::DefendersElimination};
        if (phase_timer_expired && !input.bomb_planted)
            return {RoundWinner::Defenders, RoundEndReason::TargetSaved};
        return {};
    }

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
            // Events resolve before the phase timer advances.  This preserves
            // an event outcome on the exact tick that the timer reaches zero.
            const RoundOutcome event_outcome = resolveLiveOutcome(input, false);
            if (event_outcome.resolved()) {
                endRound(event_outcome.winner, event_outcome.reason);
                break;
            }
            if (timer.tick(dt)) {
                const RoundOutcome deadline_outcome = resolveLiveOutcome(input, true);
                if (deadline_outcome.resolved()) {
                    endRound(deadline_outcome.winner, deadline_outcome.reason);
                } else if (input.bomb_planted) {
                    timer.start(config.round_seconds); // planted: round timer no longer ends round
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
    // Competitive protocol messages carry a signed 32-bit balance. Keep the
    // game-facing view representable even if a future generic wallet owner
    // supplies a wider balance directly.
    int32_t money() const {
        return static_cast<int32_t>(std::clamp<int64_t>(wallet.balance, 0,
            std::numeric_limits<int32_t>::max()));
    }
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
        return static_cast<int32_t>(m.wallet.add(def ? def->kill_award : config.uncatalogued_kill));
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
enum class TeamSelectResult : uint8_t {
    Ok = 0,
    TeamFull = 1,
    BothFull = 2,
    PhaseClosed = 3,
    InvalidTeam = 4,
    SpawnUnavailable = 5
};

// TEAM_RESULT must describe the durable team after handling a request.  A
// permitted switch that cannot acquire a valid spawn is rejected before any
// session or TeamComponent mutation, so report the prior authoritative side.
struct TeamSelectReply {
    TeamSelectResult result = TeamSelectResult::InvalidTeam;
    FpsTeam team = FpsTeam::None;
};

inline TeamSelectReply rejectedTeamSelectForUnavailableSpawn(FpsTeam authoritative_team) {
    return {TeamSelectResult::SpawnUnavailable, authoritative_team};
}

inline TeamSelectResult validateTeamSelect(FpsTeam desired, bool auto_assign,
                                           int32_t attackers, int32_t defenders,
                                           RoundPhase phase, const ModeConfig& cfg,
                                           FpsTeam& out_team,
                                           bool allow_live_admission = false) {
    out_team = FpsTeam::None;
    // Switches remain closed outside a spawn boundary. A newly admitted,
    // pawnless player may select a team during Live and wait for Freeze.
    const bool regular_selection_phase = phase == RoundPhase::WaitingForPlayers ||
        phase == RoundPhase::Warmup || phase == RoundPhase::Freeze;
    if (!regular_selection_phase && !(allow_live_admission && phase == RoundPhase::Live))
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

// --- Server session flow ---------------------------------------------------------
// Connection transport owns peer authentication and client ids.  This template
// coordinator owns the game-level consequence of an admitted client: team
// choice, whether a pawn may exist, and the transition caused by teardown.
// It intentionally has no reconnect credential or wire state; the existing
// TEAM_RESULT and player spawn messages remain the published contract.
enum class SessionState : uint8_t {
    AwaitingTeam = 0,
    AwaitingSpawn,
    Active,
    Spectating,
};

struct SessionRecord {
    uint16_t client_id = 0;
    FpsTeam team = FpsTeam::None;
    SessionState state = SessionState::AwaitingTeam;
    uint32_t last_team_select_tick = 0;
    bool has_team_select_tick = false;
};

class SessionCoordinator {
public:
    // A compact server-tick throttle prevents duplicate/replayed selection
    // bursts from repeatedly mutating a pawn.  It does not create a protocol
    // result value because TEAM_RESULT has no rate-limit enum.
    static constexpr uint32_t kTeamSelectIntervalTicks = 1;

    bool admit(uint16_t client_id) {
        if (client_id == 0 || sessions_.find(client_id) != sessions_.end())
            return false;
        SessionRecord record;
        record.client_id = client_id;
        sessions_.emplace(client_id, record);
        return true;
    }

    bool contains(uint16_t client_id) const {
        return sessions_.find(client_id) != sessions_.end();
    }

    const SessionRecord* find(uint16_t client_id) const {
        const auto it = sessions_.find(client_id);
        return it == sessions_.end() ? nullptr : &it->second;
    }

    SessionRecord* find(uint16_t client_id) {
        const auto it = sessions_.find(client_id);
        return it == sessions_.end() ? nullptr : &it->second;
    }

    bool consumeTeamSelectRequest(uint16_t client_id, uint32_t server_tick) {
        SessionRecord* record = find(client_id);
        if (record == nullptr)
            return false;
        if (record->has_team_select_tick &&
            static_cast<uint32_t>(server_tick - record->last_team_select_tick) < kTeamSelectIntervalTicks)
            return false;
        record->last_team_select_tick = server_tick;
        record->has_team_select_tick = true;
        return true;
    }

    bool assignTeam(uint16_t client_id, FpsTeam team, RoundPhase phase) {
        SessionRecord* record = find(client_id);
        if (record == nullptr || (team != FpsTeam::Spectator && !isPlayingTeam(team)))
            return false;
        record->team = team;
        record->state = team == FpsTeam::Spectator
            ? SessionState::Spectating
            : (canSpawnNow(phase) ? SessionState::Active : SessionState::AwaitingSpawn);
        return true;
    }

    bool markSpawned(uint16_t client_id, FpsTeam team, RoundPhase phase) {
        if (!assignTeam(client_id, team, phase) || !isPlayingTeam(team))
            return false;
        SessionRecord* record = find(client_id);
        record->state = canSpawnNow(phase) ? SessionState::Active : SessionState::AwaitingSpawn;
        return true;
    }

    bool markAwaitingSpawn(uint16_t client_id) {
        SessionRecord* record = find(client_id);
        if (record == nullptr || !isPlayingTeam(record->team))
            return false;
        record->state = SessionState::AwaitingSpawn;
        return true;
    }

    void playingTeamCounts(int32_t& attackers, int32_t& defenders) const {
        attackers = 0;
        defenders = 0;
        for (const auto& entry : sessions_) {
            if (entry.second.team == FpsTeam::Attackers) ++attackers;
            else if (entry.second.team == FpsTeam::Defenders) ++defenders;
        }
    }

    std::vector<uint16_t> awaitingSpawnClients() const {
        std::vector<uint16_t> clients;
        for (const auto& entry : sessions_) {
            if (entry.second.state == SessionState::AwaitingSpawn &&
                isPlayingTeam(entry.second.team))
                clients.push_back(entry.first);
        }
        std::sort(clients.begin(), clients.end());
        return clients;
    }

    void swapPlayingTeams() {
        for (auto& entry : sessions_) {
            if (entry.second.team == FpsTeam::Attackers)
                entry.second.team = FpsTeam::Defenders;
            else if (entry.second.team == FpsTeam::Defenders)
                entry.second.team = FpsTeam::Attackers;
        }
    }

    void remove(uint16_t client_id) { sessions_.erase(client_id); }
    void clear() { sessions_.clear(); }
    size_t size() const { return sessions_.size(); }

private:
    std::unordered_map<uint16_t, SessionRecord> sessions_;
};

// Server ingress uses this before mutating an ECS TeamComponent or session.
// The Live exception belongs only to a first team choice: an existing pawn or
// any session that has already chosen a team stays subject to the normal phase
// gate, so it cannot use a late join request as a team-switch path.
inline TeamSelectResult validateSessionTeamSelect(const SessionCoordinator& sessions,
                                                  uint16_t client_id,
                                                  bool has_player_entity,
                                                  FpsTeam desired,
                                                  bool auto_assign,
                                                  int32_t attackers,
                                                  int32_t defenders,
                                                  RoundPhase phase,
                                                  const ModeConfig& cfg,
                                                  FpsTeam& out_team) {
    const SessionRecord* record = sessions.find(client_id);
    const bool initial_admission = record != nullptr &&
        !has_player_entity &&
        record->state == SessionState::AwaitingTeam &&
        record->team == FpsTeam::None;
    return validateTeamSelect(desired, auto_assign, attackers, defenders, phase,
                              cfg, out_team, initial_admission);
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
    // out_dropped. Request sequences start at one and are exactly-once: a
    // malformed zero or repeated sequence returns DuplicateRequest without
    // touching the purchase state.
    BuyResult buy(const BuyContext& ctx, uint16_t item_id, FpsLogic::DroppedItem* out_dropped = nullptr) {
        // COUNT sentinel: caller spawns out_dropped only when type != COUNT.
        if (out_dropped) *out_dropped = FpsLogic::DroppedItem{WeaponType::COUNT, 0, 0};
        if (ctx.request_seq == 0)
            return BuyResult::DuplicateRequest;
        // Dedup first: replayed seq never re-charges / re-gives.
        auto seen = last_seq_.find(ctx.client_id);
        if (seen != last_seq_.end() && ctx.request_seq <= seen->second)
            return BuyResult::DuplicateRequest;
        BuyResult result = buyImpl(ctx, item_id, out_dropped);
        last_seq_[ctx.client_id] = ctx.request_seq;
        last_result_[ctx.client_id] = result;
        return result;
    }
    BuyResult lastResultFor(uint16_t client_id) const {
        auto it = last_result_.find(client_id);
        return it == last_result_.end() ? BuyResult::UnknownItem : it->second;
    }
    void clearClient(uint16_t client_id) {
        last_seq_.erase(client_id);
        last_result_.erase(client_id);
    }

private:
    std::unordered_map<uint16_t, uint32_t> last_seq_;
    std::unordered_map<uint16_t, BuyResult> last_result_;

    BuyResult buyImpl(const BuyContext& ctx, uint16_t item_id, FpsLogic::DroppedItem* out_dropped) {
        const WeaponCatalogDef* def = findCatalogById(item_id);
        if (!def || !isCatalogBuyable(*def)) return BuyResult::UnknownItem;
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

        // Reject incomplete or inconsistent catalog rows before changing any
        // player state. The static table is shared by client and server, but
        // this keeps a future bad row from becoming a charge-without-grant.
        int slot = static_cast<int>(def->slot);
        const bool equipment = def->kind == BuyItemKind::Equipment;
        if (def->kind != BuyItemKind::Weapon && def->kind != BuyItemKind::Grenade && !equipment) {
            return BuyResult::UnknownItem;
        } else if (equipment) {
            if (!isCatalogDataOnly(*def) || (def->id != 42 && def->id != 43 && def->id != 44))
                return BuyResult::UnknownItem;
        } else if (isCatalogDataOnly(*def) || !FpsLogic::isValidLoadoutSlot(slot) ||
                   !FpsLogic::isValidWeaponType(def->weapon_type) ||
                   FpsLogic::slotForWeaponType(def->weapon_type) != def->slot) {
            return BuyResult::UnknownItem;
        }

        // Work on copies until every validation and loadout operation has
        // succeeded. Rejected requests therefore cannot leave a changed
        // wallet, loadout, mirrored inventory, active weapon, or equipment.
        MoneyComponent staged_money = *ctx.money;
        InventoryComponent staged_inventory = *ctx.inventory;
        PlayerLoadoutComponent staged_loadout = *ctx.loadout;
        WeaponComponent staged_weapon = *ctx.weapon;
        ArmorComponent staged_armor{};
        if (ctx.armor) staged_armor = *ctx.armor;
        FpsLogic::normalizeLoadout(staged_loadout);

        // Slot / ownership pre-checks (no live-state mutation).
        if (def->kind == BuyItemKind::Grenade) {
            if (staged_loadout.slots[slot].occupied) return BuyResult::CarryLimit;
        }
        if (equipment) {
            if (!ctx.armor) return BuyResult::UnknownItem;
            if (def->id == 42 && staged_armor.armor >= 100) return BuyResult::AlreadyOwned;
            if (def->id == 43 && staged_armor.armor >= 100 && staged_armor.has_helmet) return BuyResult::AlreadyOwned;
            if (def->id == 44 && staged_armor.has_defuse_kit) return BuyResult::AlreadyOwned;
        }
        if (!staged_money.wallet.trySpend(def->price)) return BuyResult::InsufficientFunds;

        FpsLogic::DroppedItem staged_dropped{WeaponType::COUNT, 0, 0};
        if (equipment) {
            if (def->id == 42) staged_armor.armor = 100;
            else if (def->id == 43) { staged_armor.armor = 100; staged_armor.has_helmet = true; }
            else staged_armor.has_defuse_kit = true;
        } else {
            // Weapon / grenade into its slot; an occupied primary/secondary is
            // replaced (old weapon reported as dropped), per spec S4.5.
            auto& target = staged_loadout.slots[slot];
            if (target.occupied)
                staged_dropped = FpsLogic::DroppedItem{target.type, target.ammo, target.reserve};
            const bool was_active = staged_loadout.active_slot == slot;
            target = WeaponSlotState{def->weapon_type, def->mag, def->reserve, true};
            if (staged_loadout.active_slot < 0) staged_loadout.active_slot = slot;
            if (was_active && !FpsLogic::loadSlotIntoWeapon(staged_loadout, staged_weapon, slot))
                return BuyResult::UnknownItem;
            FpsLogic::syncInventory(staged_inventory, staged_loadout);
        }

        *ctx.money = staged_money;
        *ctx.inventory = staged_inventory;
        *ctx.loadout = staged_loadout;
        *ctx.weapon = staged_weapon;
        if (ctx.armor) *ctx.armor = staged_armor;
        if (out_dropped) *out_dropped = staged_dropped;
        return BuyResult::Ok;
    }
};

enum class EconomyTransitionKind : uint8_t {
    None = 0,
    Buy,
    DeathKill,
    DeathSuicide,
    BombPlanter,
    BombTeam,
    BombDefuse,
    RoundEnd,
    PeriodReset,
};

// This record is diagnostic state only.  It lets the server and pure tests
// verify that a world applied the expected economy hook without creating a
// second wallet or a parallel match authority.
struct EconomyTransitionState {
    uint64_t count = 0;
    EconomyTransitionKind last_kind = EconomyTransitionKind::None;
    int32_t last_amount = 0;
};

// All mutable competitive authority belongs to the loaded server world.  The
// server stores this in that world's registry context, so a level restart or a
// second world cannot inherit a previous match, bomb channel, input edge, or
// buy-request history.  The domain systems remain transition classes; this is
// their ownership boundary rather than a second copy of their rules.
struct CompetitiveWorldContext {
    bool enabled = false;
    RoundSystem round;
    BuySystem buy;
    EconomySystem economy;
    BombSystem bomb;
    BombHooksAdapter bomb_hooks;
    SessionCoordinator sessions;
    EconomyTransitionState economy_transitions;
    entt::entity match_entity = entt::null;
    std::unordered_map<uint16_t, bool> bomb_use_held;
    std::unordered_map<uint16_t, uint16_t> bomb_last_buttons;
    std::unordered_map<uint32_t, int32_t> bomb_prev_health;
    std::unordered_map<uint32_t, uint8_t> bomb_prev_weapon;
    bool bomb_sites_authored = false;
    float round_heartbeat = 0.0f;

    CompetitiveWorldContext() : bomb_hooks(&bomb) {
        round.bomb_hooks = &bomb_hooks;
    }

    void configure(const ModeConfig& mode_config, const EconomyConfig& economy_config = EconomyConfig{}) {
        clear();
        enabled = true;
        round.configure(mode_config);
        round.bomb_hooks = &bomb_hooks;
        buy.config = mode_config;
        economy.config = economy_config;
    }

    BuyResult onBuy(const BuyContext& context, uint16_t item_id, FpsLogic::DroppedItem* out_dropped = nullptr) {
        const int32_t before = context.money ? context.money->money() : 0;
        const BuyResult result = buy.buy(context, item_id, out_dropped);
        if (result == BuyResult::Ok && context.money)
            recordEconomyTransition(EconomyTransitionKind::Buy, context.money->money() - before);
        return result;
    }

    int32_t onDeath(MoneyComponent* victim, MoneyComponent* killer, bool self_inflicted,
                    const WeaponCatalogDef* weapon_def) {
        if (self_inflicted) {
            if (!victim)
                return 0;
            const int32_t amount = economy.applySuicidePenalty(*victim);
            recordEconomyTransition(EconomyTransitionKind::DeathSuicide, amount);
            return amount;
        }
        if (!killer)
            return 0;
        const int32_t amount = economy.awardKill(*killer, weapon_def);
        recordEconomyTransition(EconomyTransitionKind::DeathKill, amount);
        return amount;
    }

    int32_t onBombPlanter(MoneyComponent& money) {
        const int32_t amount = economy.awardPlantPlanter(money);
        recordEconomyTransition(EconomyTransitionKind::BombPlanter, amount);
        return amount;
    }

    int32_t onBombTeam(MoneyComponent& money) {
        const int32_t amount = economy.awardPlantTeam(money);
        recordEconomyTransition(EconomyTransitionKind::BombTeam, amount);
        return amount;
    }

    int32_t onBombDefuse(MoneyComponent& money) {
        const int32_t amount = economy.awardDefuse(money);
        recordEconomyTransition(EconomyTransitionKind::BombDefuse, amount);
        return amount;
    }

    int32_t onRoundEnd(MoneyComponent& money, FpsTeam player_team,
                       RoundWinner winner, RoundEndReason reason) {
        const int32_t amount = economy.awardRoundEnd(money, player_team, winner, reason);
        recordEconomyTransition(EconomyTransitionKind::RoundEnd, amount);
        return amount;
    }

    void onPeriodReset(MoneyComponent& money, bool overtime) {
        const int32_t before = money.money();
        money.reset(round.config.currentStartMoney(overtime), round.config.money_cap);
        recordEconomyTransition(EconomyTransitionKind::PeriodReset, money.money() - before);
    }

    void onMatchStart(MoneyComponent& money) {
        const int32_t before = money.money();
        money.reset(round.config.currentStartMoney(false), round.config.money_cap);
        recordEconomyTransition(EconomyTransitionKind::PeriodReset, money.money() - before);
    }

    // RoundSystem is the canonical match authority.  This entity is its
    // one-way ECS publication for replication and systems that consume the
    // match snapshot.  Keeping publication here makes the world boundary
    // explicit and avoids a second writable match state.
    void createMatchSnapshot(entt::registry& registry) {
        if (!registry.valid(match_entity))
            match_entity = registry.create();
        syncMatchSnapshot(registry);
    }

    bool syncMatchSnapshot(entt::registry& registry) const {
        if (!registry.valid(match_entity))
            return false;
        registry.get_or_emplace<MatchStateComponent>(match_entity) = round.state;
        registry.get_or_emplace<CountdownTimerComponent>(match_entity) = round.timer;
        return true;
    }

    bool snapshotMatches(const entt::registry& registry) const {
        if (!registry.valid(match_entity) ||
            !registry.all_of<MatchStateComponent, CountdownTimerComponent>(match_entity))
            return false;

        const auto& snapshot = registry.get<MatchStateComponent>(match_entity);
        const auto& snapshot_timer = registry.get<CountdownTimerComponent>(match_entity);
        const auto& canonical = round.state;
        return snapshot.phase == canonical.phase &&
            snapshot.phase_time_remaining == canonical.phase_time_remaining &&
            snapshot.phase_deadline_tick == canonical.phase_deadline_tick &&
            snapshot.server_tick == canonical.server_tick &&
            snapshot.round_number == canonical.round_number &&
            snapshot.wins_attackers == canonical.wins_attackers &&
            snapshot.wins_defenders == canonical.wins_defenders &&
            snapshot.ot_wins_attackers == canonical.ot_wins_attackers &&
            snapshot.ot_wins_defenders == canonical.ot_wins_defenders &&
            snapshot.first_half_wins_attackers == canonical.first_half_wins_attackers &&
            snapshot.first_half_wins_defenders == canonical.first_half_wins_defenders &&
            snapshot.bomb_planted == canonical.bomb_planted &&
            snapshot.overtime == canonical.overtime &&
            snapshot.paused == canonical.paused &&
            snapshot.teams_swapped == canonical.teams_swapped &&
            snapshot.last_reason == canonical.last_reason &&
            snapshot.last_winner == canonical.last_winner &&
            snapshot.match_winner == canonical.match_winner &&
            snapshot_timer.duration == round.timer.duration &&
            snapshot_timer.remaining == round.timer.remaining &&
            snapshot_timer.running == round.timer.running &&
            snapshot_timer.loop == round.timer.loop;
    }

    void destroyMatchSnapshot(entt::registry& registry) {
        if (registry.valid(match_entity))
            registry.destroy(match_entity);
        clear();
    }

    void clear() {
        enabled = false;
        round = RoundSystem{};
        buy = BuySystem{};
        economy = EconomySystem{};
        bomb = BombSystem{};
        bomb_hooks.system = &bomb;
        round.bomb_hooks = &bomb_hooks;
        sessions.clear();
        economy_transitions = EconomyTransitionState{};
        match_entity = entt::null;
        bomb_use_held.clear();
        bomb_last_buttons.clear();
        bomb_prev_health.clear();
        bomb_prev_weapon.clear();
        bomb_sites_authored = false;
        round_heartbeat = 0.0f;
    }

private:
    void recordEconomyTransition(EconomyTransitionKind kind, int32_t amount) {
        ++economy_transitions.count;
        economy_transitions.last_kind = kind;
        economy_transitions.last_amount = amount;
    }
};

inline void clearCompetitiveWorldContext(entt::registry& registry)
{
    if (auto* context = registry.ctx().find<CompetitiveWorldContext>()) {
        context->destroyMatchSnapshot(registry);
        registry.ctx().erase<CompetitiveWorldContext>();
    }

    // Wallet and equipment are durable player ECS state while a competitive
    // world is running.  A world teardown must remove that mode-owned state
    // as well: otherwise reusing a registry for a new level or a score mode
    // can leave an old wallet behind after the round/bomb runtime is gone.
    // Copy entities before removing components because EnTT views are
    // invalidated by the removal.
    std::vector<entt::entity> players;
    auto money_view = registry.view<MoneyComponent>();
    for (const entt::entity entity : money_view)
        players.push_back(entity);
    for (const entt::entity entity : players) {
        if (registry.valid(entity))
            registry.remove<MoneyComponent>(entity);
    }

    players.clear();
    auto armor_view = registry.view<ArmorComponent>();
    for (const entt::entity entity : armor_view)
        players.push_back(entity);
    for (const entt::entity entity : players) {
        if (registry.valid(entity))
            registry.remove<ArmorComponent>(entity);
    }
}

// These three helpers are the FPSShooter template's explicit lifecycle
// boundary.  They intentionally operate on an existing world registry rather
// than adding a generic engine scheduler or singleton.  The owning server
// calls start after it has selected competitive mode, tick once per gameplay
// update, and stop before the world is released or reused.
inline CompetitiveWorldContext& startCompetitiveWorldRuntime(
    entt::registry& registry,
    const ModeConfig& mode_config,
    const EconomyConfig& economy_config = EconomyConfig{})
{
    clearCompetitiveWorldContext(registry);
    auto& context = registry.ctx().emplace<CompetitiveWorldContext>();
    context.configure(mode_config, economy_config);
    context.createMatchSnapshot(registry);
    return context;
}

inline bool tickCompetitiveWorldRuntime(
    entt::registry& registry,
    float delta_time,
    const RoundTickInput& input)
{
    CompetitiveWorldContext* context = registry.ctx().find<CompetitiveWorldContext>();
    if (context == nullptr || !context->enabled)
        return false;
    context->round.tick(delta_time, input);
    return context->syncMatchSnapshot(registry);
}

inline void stopCompetitiveWorldRuntime(entt::registry& registry)
{
    clearCompetitiveWorldContext(registry);
}

} // namespace Competitive
