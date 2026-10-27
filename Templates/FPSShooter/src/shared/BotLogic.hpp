#pragma once
// FPSShooter bots v1: pure, testable bot logic (game layer only).
// Decision code consumes only BotPerceptComponent/blackboard/match-state
// summaries passed in BotDecisionInput -- never raw enemy transforms.
#include "CompetitiveLogic.hpp"
#include "Network/NetworkProtocol.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Bots {

inline constexpr uint16_t kBotClientIdBase = 40000;
inline uint8_t clampDifficulty(int32_t d) { return (uint8_t)std::clamp<int32_t>(d, 0, 3); }

struct DifficultyRow {
    float reaction_time, aim_error_deg, aim_turn_rate_deg, burst_pause, view_distance, fov_deg, hearing_scale, memory_time, strafe_interval;
    int32_t burst_len, retreat_hp;
};
inline constexpr DifficultyRow kDifficultyTable[4] = {
    {0.60f, 6.0f, 180.0f, 0.45f, 45.0f,  90.0f, 0.70f,  2.0f, 0.0f, 3, 0},
    {0.40f, 3.5f, 270.0f, 0.30f, 60.0f, 100.0f, 1.00f,  4.0f, 0.9f, 4, 25},
    {0.25f, 1.8f, 400.0f, 0.20f, 75.0f, 110.0f, 1.15f,  7.0f, 0.7f, 5, 30},
    {0.15f, 0.8f, 540.0f, 0.12f, 90.0f, 120.0f, 1.30f, 10.0f, 0.5f, 6, 35},
};
inline const DifficultyRow& difficultyFor(uint8_t d) { return kDifficultyTable[clampDifficulty(d)]; }

inline uint32_t rngNext(uint32_t& s) { if (!s) s = 1; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float rngUniform(uint32_t& s) { return (rngNext(s) >> 8) * (1.0f / 16777216.0f); } // [0,1)
inline float rngSigned(uint32_t& s) { return rngUniform(s) * 2.0f - 1.0f; }

enum class BotState : uint8_t { Idle, Buy, Advance, Plant, Defend, Defuse, Engage, Retreat, Spectate };
struct NavAgentComponent {
    float max_speed = 5.0f, max_accel = 20.0f, arrive_radius = 1.2f, waypoint_radius = 0.6f, repath_cooldown = 0.25f;
    std::vector<glm::vec3> path; uint32_t path_index = 0;
    glm::vec3 goal{0}; bool has_goal = false; bool path_valid = false;
    float repath_timer = 0; glm::vec3 desired_velocity{0}; bool stuck = false;
    int32_t path_failures = 0;
    // A request is owned by this bot until the world scheduler grants or
    // rejects it.  The tick is deliberately data rather than wall-clock time:
    // replay order remains stable regardless of frame duration.
    bool path_request_pending = false;
    uint32_t path_request_tick = 0;
};
struct SteeringComponent { glm::vec3 velocity{0}; float stuck_time = 0; glm::vec3 last_pos{0}; bool has_last = false; uint8_t unstuck_phase = 0; float unstuck_timer = 0; float stuck_window_time = 0; glm::vec3 window_start{0}; };
struct BotComponent {
    uint8_t difficulty = 1; BotState state = BotState::Idle; BotState resume_state = BotState::Advance; bool has_resume = false;
    float state_time = 0, next_decision_time = 0, engage_lost_time = 0, aim_yaw = 0, aim_pitch = 0;
    float strafe_timer = 0; int8_t strafe_dir = 1; // Engage strafe pattern (bots.md: flips every strafe_interval)
    uint32_t rng_state = 1; entt::entity pawn = entt::null; int32_t bot_index = 0;
    bool buy_done = false;
};
struct BotPerceptComponent {
    entt::entity visible_enemy = entt::null; glm::vec3 last_seen_pos{0}; float last_seen_age = 1e9f;
    float visible_fraction = 0, confirm_timer = 0;
    glm::vec3 heard_pos{0}; float heard_age = 1e9f, heard_loudness = 0;
    bool bomb_planted_heard = false, bomb_beep_heard = false, sees_bomb_carrier = false, sees_planted_bomb = false;
};
struct BotIntentComponent {
    glm::vec3 move_goal{0}; bool has_move_goal = false;
    bool want_crouch = false, want_jump = false, want_use = false, want_attack = false;
    float aim_yaw = 0, aim_pitch = 0; uint8_t want_weapon_slot = 0; int32_t buy_request_id = -1;
};
struct TeamBlackboardComponent { uint8_t team = 0; glm::vec3 last_contact_pos{0}; float last_contact_age = 1e9f; int8_t planned_site = -1; };

// ---- Perception ----
// blocked(origin, point) -> true when world geometry blocks that sample.
struct EnemyCandidate { entt::entity entity = entt::null; glm::vec3 position{0}; };
using BlockedFn = std::function<bool(const glm::vec3&, const glm::vec3&)>;
inline glm::vec3 forwardFromYaw(float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); }

// The current target is a short-lived sight result. It must not survive an
// entity lifecycle/team/alive change while the server is between sight passes.
// Memory deliberately retains no entity identity, only a bounded position from
// an already visible observation for future, explicitly authored behavior.
inline void clearCurrentSight(BotPerceptComponent& p) {
    p.visible_enemy = entt::null;
    p.visible_fraction = 0.0f;
    p.confirm_timer = 0.0f;
}

inline void forgetSightMemory(BotPerceptComponent& p) {
    p.last_seen_pos = glm::vec3(0.0f);
    p.last_seen_age = 1e9f;
}

inline bool hasSightMemory(const BotPerceptComponent& p, uint8_t difficulty) {
    return p.last_seen_age < difficultyFor(difficulty).memory_time;
}

inline void advancePerceptionClock(BotPerceptComponent& p, uint8_t difficulty, float dt) {
    const float elapsed = std::max(dt, 0.0f);
    p.last_seen_age += elapsed;
    p.heard_age += elapsed;
    if (!hasSightMemory(p, difficulty)) forgetSightMemory(p);
}

inline void pruneCurrentSight(BotPerceptComponent& p, bool target_still_eligible) {
    if (p.visible_enemy != entt::null && !target_still_eligible) {
        clearCurrentSight(p);
        forgetSightMemory(p);
    }
}

inline void updatePerception(BotPerceptComponent& p, const glm::vec3& eye, float yaw,
                             const std::vector<EnemyCandidate>& enemies, uint8_t difficulty, float dt, const BlockedFn& blocked) {
    advancePerceptionClock(p, difficulty, dt);
    const auto& d = difficultyFor(difficulty);
    entt::entity best = entt::null; float best_frac = 0; glm::vec3 best_pos{0};
    glm::vec3 fwd = forwardFromYaw(yaw);
    for (const auto& e : enemies) {
        if (e.entity == entt::null || !std::isfinite(e.position.x) || !std::isfinite(e.position.y) || !std::isfinite(e.position.z)) continue;
        glm::vec3 to = e.position - eye; float dist = glm::length(to);
        if (dist > d.view_distance || dist < 0.001f) continue;
        glm::vec3 flat(to.x, 0, to.z); float flat_len = glm::length(flat);
        if (flat_len > 0.001f) {
            float cosang = glm::dot(fwd, flat / flat_len);
            float ang = std::acos(std::clamp(cosang, -1.0f, 1.0f));
            if (ang > glm::radians(d.fov_deg) * 0.5f && dist > 3.0f) continue;
        }
        int vis = 0;
        const float offs[3] = {0.3f, 1.35f, 1.65f};
        for (float o : offs) { glm::vec3 pt = e.position + glm::vec3(0, o, 0); if (!blocked || !blocked(eye, pt)) ++vis; }
        float frac = vis / 3.0f;
        // Candidate collection can come from an ECS view. Preserve replayable
        // selection when two visible candidates have the same coverage.
        if (frac > best_frac ||
            (frac == best_frac && frac > 0.0f &&
             (best == entt::null || entt::to_integral(e.entity) < entt::to_integral(best)))) {
            best_frac = frac;
            best = e.entity;
            best_pos = e.position;
        }
    }
    p.visible_fraction = best_frac;
    if (best_frac > 0.0f) {
        if (p.visible_enemy != best) p.confirm_timer = 0.0f;
        p.visible_enemy = best;
        p.last_seen_pos = best_pos;
        p.last_seen_age = 0.0f;
        p.confirm_timer += std::max(dt, 0.0f);
    } else {
        clearCurrentSight(p);
    }
}
inline bool enemyConfirmed(const BotPerceptComponent& p, uint8_t difficulty) {
    return p.visible_fraction > 0.0f && p.confirm_timer >= difficultyFor(difficulty).reaction_time;
}

// ---- Steering (no teleport: only desired velocity) ----
inline void setPath(NavAgentComponent& n, const std::vector<glm::vec3>& pts, const glm::vec3& goal) {
    n.path = pts; n.goal = goal; n.has_goal = true; n.path_valid = !pts.empty(); n.path_index = pts.size() > 1 ? 1 : 0; n.path_failures = pts.empty() ? n.path_failures + 1 : 0;
    n.path_request_pending = false;
    n.path_request_tick = 0;
}

// ---- Bounded path scheduling ----
// The server owns query execution, because it owns the per-world NavMesh.
// These data helpers make selection deterministic and keep all motion on the
// ordinary input path.  Older requests win; ties are broken by the stable bot
// index and then owner id, never ECS-view iteration order.
inline constexpr uint32_t kBotPathRequestsPerTick = 2;

struct PathScheduleRequest {
    int32_t bot_index = 0;
    uint16_t owner_client_id = 0;
    uint32_t queued_tick = 0;
};

inline bool pathRequestEarlier(const PathScheduleRequest& a, const PathScheduleRequest& b) {
    if (a.queued_tick != b.queued_tick) return a.queued_tick < b.queued_tick;
    if (a.bot_index != b.bot_index) return a.bot_index < b.bot_index;
    return a.owner_client_id < b.owner_client_id;
}

inline std::vector<PathScheduleRequest> selectPathRequests(std::vector<PathScheduleRequest> pending,
                                                            uint32_t budget = kBotPathRequestsPerTick) {
    std::sort(pending.begin(), pending.end(), pathRequestEarlier);
    if (pending.size() > budget) pending.resize(budget);
    return pending;
}

inline bool pathGoalChanged(const NavAgentComponent& n, const glm::vec3& goal) {
    glm::vec3 delta = goal - n.goal;
    delta.y = 0.0f;
    return !n.has_goal || !n.path_valid || glm::length(delta) > 2.0f || n.stuck;
}

inline void queuePathRequest(NavAgentComponent& n, uint32_t tick) {
    if (!n.path_request_pending) {
        n.path_request_pending = true;
        n.path_request_tick = tick;
    }
}

inline void cancelPathRequest(NavAgentComponent& n) {
    n.path_request_pending = false;
    n.path_request_tick = 0;
}

inline float pathFailureBackoff(const NavAgentComponent& n) {
    const int32_t exponent = std::clamp(n.path_failures, 0, 3);
    return std::min(n.repath_cooldown * static_cast<float>(1u << exponent), 2.0f);
}

inline void rejectPathRequest(NavAgentComponent& n) {
    cancelPathRequest(n);
    n.path.clear();
    n.path_index = 0;
    n.path_valid = false;
    n.has_goal = false;
    n.desired_velocity = glm::vec3(0.0f);
    ++n.path_failures;
    n.repath_timer = pathFailureBackoff(n);
}

inline void acceptPathRequest(NavAgentComponent& n, const std::vector<glm::vec3>& points,
                              const glm::vec3& resolved_goal, SteeringComponent& s) {
    setPath(n, points, resolved_goal);
    n.repath_timer = n.repath_cooldown;
    n.stuck = false;
    s.stuck_time = 0.0f;
    s.stuck_window_time = 0.0f;
    s.window_start = s.last_pos;
}
inline void updateSteering(NavAgentComponent& n, SteeringComponent& s, const glm::vec3& pos, float dt) {
    n.desired_velocity = glm::vec3(0);
    if (!s.has_last) { s.last_pos = pos; s.window_start = pos; s.has_last = true; }
    if (!n.has_goal || !n.path_valid || n.path.empty()) { s.velocity = glm::vec3(0); return; }
    while (n.path_index < n.path.size()) {
        glm::vec3 to = n.path[n.path_index] - pos; to.y = 0;
        if (glm::length(to) < n.waypoint_radius) { ++n.path_index; continue; } break;
    }
    if (n.path_index >= n.path.size()) { n.has_goal = false; s.velocity = glm::vec3(0); return; }
    glm::vec3 target = n.path[n.path_index];
    glm::vec3 to = target - pos; to.y = 0; float dist = glm::length(to);
    glm::vec3 final_to = n.goal - pos; final_to.y = 0; float final_dist = glm::length(final_to);
    float arrive = std::clamp(final_dist / std::max(n.arrive_radius, 0.01f), 0.25f, 1.0f);
    glm::vec3 desired = dist > 0.001f ? (to / dist) * n.max_speed * arrive : glm::vec3(0);
    // move_towards smoothing
    glm::vec3 delta = desired - s.velocity; float max_delta = n.max_accel * dt; float len = glm::length(delta);
    s.velocity = len > max_delta && len > 0.0f ? s.velocity + delta / len * max_delta : desired;
    n.desired_velocity = s.velocity;
    // Stuck: commanded >1 m/s but <0.15 m displacement over 0.75 s window.
    s.stuck_window_time += dt;
    if (s.stuck_window_time >= 0.75f) {
        glm::vec3 disp = pos - s.window_start; disp.y = 0;
        n.stuck = glm::length(n.desired_velocity) > 1.0f && glm::length(disp) < 0.15f;
        s.stuck_time = n.stuck ? s.stuck_time + s.stuck_window_time : 0.0f;
        s.window_start = pos; s.stuck_window_time = 0;
    }
    s.last_pos = pos;
}

// ---- AI debug v1 (bots.md section 10; inert unless caller wires it) ----
inline const char* botStateName(BotState s) {
    switch (s) {
    case BotState::Idle: return "Idle"; case BotState::Buy: return "Buy"; case BotState::Advance: return "Advance";
    case BotState::Plant: return "Plant"; case BotState::Defend: return "Defend"; case BotState::Defuse: return "Defuse";
    case BotState::Engage: return "Engage"; case BotState::Retreat: return "Retreat"; case BotState::Spectate: return "Spectate";
    } return "?";
}
struct TransitionRecord { uint32_t tick = 0; int32_t bot_index = 0; BotState from = BotState::Idle; BotState to = BotState::Idle; std::string reason; };
struct TransitionRingBuffer {
    static constexpr size_t kCapacity = 64;
    std::array<TransitionRecord, kCapacity> records{}; size_t head = 0; size_t count = 0; uint64_t total_pushed = 0; // head = next write
    void push(const TransitionRecord& r) { records[head] = r; head = (head + 1) % kCapacity; if (count < kCapacity) ++count; ++total_pushed; }
    size_t size() const { return count; }
    const TransitionRecord& oldest() const { return records[(head + kCapacity - count) % kCapacity]; }
    const TransitionRecord& newest() const { return records[(head + kCapacity - 1) % kCapacity]; }
    const TransitionRecord& at(size_t i) const { return records[(head + kCapacity - count + i) % kCapacity]; } // 0=oldest
};
inline std::string formatTransition(const TransitionRecord& r) {
    char buf[256]; std::snprintf(buf, sizeof(buf), "[BotDebug] transition tick=%u bot=%d from=%s to=%s reason=%s",
        r.tick, r.bot_index, botStateName(r.from), botStateName(r.to), r.reason.c_str()); return std::string(buf);
}
struct BotDumpInput {
    int32_t bot_index = 0; int32_t team = 0; BotState state = BotState::Idle; int32_t hp = 0;
    glm::vec3 pos{0}; glm::vec3 goal{0}; bool has_goal = false; bool path_valid = false; bool stuck = false;
    float visible_age = 1e9f; float heard_age = 1e9f;
};
inline std::string formatBotDump(const BotDumpInput& d) {
    char buf[384]; std::snprintf(buf, sizeof(buf),
        "[BotDebug] bot=%d team=%d state=%s hp=%d pos=%.2f,%.2f,%.2f goal=%.2f,%.2f,%.2f path_valid=%d stuck=%d percept_visible_age=%.2f percept_heard_age=%.2f",
        d.bot_index, d.team, botStateName(d.state), d.hp, d.pos.x, d.pos.y, d.pos.z,
        d.has_goal ? d.goal.x : 0.0f, d.has_goal ? d.goal.y : 0.0f, d.has_goal ? d.goal.z : 0.0f,
        d.path_valid ? 1 : 0, d.stuck ? 1 : 0, d.visible_age, d.heard_age); return std::string(buf);
}
// ai_pause_bots: neutral sample (no movement/attack/use), camera preserved.
inline Net::InputSample synthesizeNeutralInput(uint32_t tick, float yaw = 0.0f, float pitch = 0.0f) {
    Net::InputSample s{}; s.tick = tick; s.camera_yaw = yaw; s.camera_pitch = pitch; return s;
}
struct StuckLogLatch { bool emitted = false; bool check(bool stuck) { if (!stuck) { emitted = false; return false; } if (emitted) return false; emitted = true; return true; } };

// ---- Decision FSM ----
struct BotDecisionInput {
    Competitive::RoundPhase phase = Competitive::RoundPhase::Live;
    Competitive::FpsTeam team = Competitive::FpsTeam::Attackers;
    bool alive = true, is_carrier = false, bomb_planted = false, at_site = false, at_bomb = false;
    glm::vec3 site_pos{0}; bool has_site = false; glm::vec3 bomb_pos{0}; bool has_bomb_pos = false;
    int32_t health = 100; float dt = 0.1f; glm::vec3 bot_pos{0};
};
inline void setState(BotComponent& b, BotState s) { if (b.state != s) { b.state = s; b.state_time = 0; b.engage_lost_time = 0; if (s == BotState::Engage) b.strafe_timer = 0; } }

inline void decide(BotComponent& b, const BotPerceptComponent& p, BotIntentComponent& intent, const BotDecisionInput& in,
                   TransitionRingBuffer* transitions = nullptr, uint32_t tick = 0) {
    intent = BotIntentComponent{}; intent.aim_yaw = b.aim_yaw; intent.aim_pitch = b.aim_pitch;
    b.state_time += in.dt;
    const auto& diff = difficultyFor(b.difficulty);
    auto tr = [&](BotState s, const char* reason) { if (b.state != s) { if (transitions) transitions->push(TransitionRecord{tick, b.bot_index, b.state, s, reason}); setState(b, s); } };
    if (!in.alive) { tr(BotState::Spectate, "dead"); return; }
    using P = Competitive::RoundPhase;
    if (in.phase == P::Freeze) { if (b.state == BotState::Spectate || b.state == BotState::Idle || b.state == BotState::Advance) tr(BotState::Buy, "phase_freeze"); }
    else if (in.phase == P::Warmup || in.phase == P::WaitingForPlayers) { tr(BotState::Idle, "phase_warmup"); return; }
    else if (b.state == BotState::Buy && in.phase == P::Live) tr(BotState::Advance, "phase_live");
    else if (b.state == BotState::Idle && in.phase == P::Live) tr(BotState::Advance, "phase_live");
    else if (b.state == BotState::Spectate && in.phase == P::Live) return; // dead-revived handled by alive/Freeze

    bool confirmed = enemyConfirmed(p, b.difficulty);
    // Retreat trigger (checked before Engage overlay return).
    if (b.state != BotState::Engage && b.state != BotState::Retreat && diff.retreat_hp > 0 && in.health > 0 && in.health < diff.retreat_hp && confirmed) {
        b.resume_state = BotState::Advance; b.has_resume = true; tr(BotState::Retreat, "retreat_low_hp");
    }
    // Engage overlay
    if (b.state != BotState::Engage && confirmed &&
        (b.state == BotState::Advance || b.state == BotState::Plant || b.state == BotState::Defend || b.state == BotState::Defuse)) {
        b.resume_state = b.state; b.has_resume = true; tr(BotState::Engage, "engage_confirmed");
    }
    switch (b.state) {
    case BotState::Idle: case BotState::Spectate: return;
    case BotState::Buy: intent.buy_request_id = 0; if (in.phase == P::Live) tr(BotState::Advance, "phase_live"); return;
    case BotState::Engage: {
        if (p.visible_fraction > 0.0f) { b.engage_lost_time = 0; intent.want_attack = true; glm::vec3 ad = p.last_seen_pos - in.bot_pos; intent.aim_yaw = std::atan2(ad.x, ad.z); intent.aim_pitch = std::clamp(std::atan2(1.35f, std::max(glm::length(glm::vec3(ad.x, 0, ad.z)), 0.01f)), -1.5f, 1.5f); }
        else { b.engage_lost_time += in.dt; intent.aim_yaw = b.aim_yaw; if (b.engage_lost_time > 0.75f) { BotState r = b.has_resume ? b.resume_state : BotState::Advance; b.has_resume = false; tr(r, "engage_lost"); } }
        return; }
    case BotState::Retreat:
        if (in.has_site) { intent.move_goal = in.site_pos; intent.has_move_goal = true; }
        if (b.state_time > 2.0f && !confirmed) { BotState r = b.has_resume ? b.resume_state : BotState::Advance; b.has_resume = false; tr(in.bomb_planted ? BotState::Defend : r, "retreat_safe"); }
        return;
    case BotState::Plant:
        if (in.bomb_planted) { tr(BotState::Defend, "planted"); return; }
        if (!in.is_carrier) { tr(BotState::Advance, "not_carrier"); return; }
        intent.want_use = true; return;
    case BotState::Defuse:
        if (!in.bomb_planted) { tr(BotState::Advance, "bomb_gone"); return; }
        intent.want_use = true; return;
    case BotState::Defend:
        if (in.team == Competitive::FpsTeam::Defenders && in.bomb_planted && in.at_bomb) { tr(BotState::Defuse, "at_bomb"); intent.want_use = true; }
        return;
    case BotState::Advance: default: {
        if (in.bomb_planted) {
            if (in.team == Competitive::FpsTeam::Defenders) {
                if (in.at_bomb) { tr(BotState::Defuse, "at_bomb"); intent.want_use = true; return; }
                if (in.has_bomb_pos) { intent.move_goal = in.bomb_pos; intent.has_move_goal = true; }
                return;
            }
            tr(BotState::Defend, "planted"); return;
        }
        if (in.is_carrier && in.at_site) { tr(BotState::Plant, "carrier_at_site"); intent.want_use = true; return; }
        if (in.has_site) { intent.move_goal = in.site_pos; intent.has_move_goal = true; }
        return; }
    }
}

// ---- Buy planning -------------------------------------------------------------
// Purchase intent is catalog-ID based.  The static catalog is the source of
// truth for prices, team ownership and item shape; bot policy must not carry a
// second alias or price table.  A group has a small, deterministic list of
// alternatives.  The server tries each ID at most once through BuySystem and
// never repeats a rejected request.
enum class BuyPlanKind : uint8_t { Pistol, Force, Full, Eco, Save };
enum class BuyPlanGroup : uint8_t { Primary, Armor, TeamEquipment, Grenade, Sidearm, Count };
struct BuyPlanRequest { uint16_t item_id = 0; BuyPlanGroup group = BuyPlanGroup::Primary; };
struct BuyPlan {
    BuyPlanKind kind = BuyPlanKind::Save;
    std::vector<BuyPlanRequest> requests;
};

inline constexpr size_t kBotBuyAttemptsPerGroup = 4;

inline uint8_t catalogTeamBit(Competitive::FpsTeam team) {
    return team == Competitive::FpsTeam::Attackers ? kCatalogTeamAttackers
         : team == Competitive::FpsTeam::Defenders ? kCatalogTeamDefenders : 0;
}

inline bool catalogAvailableToBot(const WeaponCatalogDef& def, Competitive::FpsTeam team) {
    const uint8_t team_bit = catalogTeamBit(team);
    return team_bit != 0 && isCatalogBuyable(def) && (def.team_mask & team_bit) != 0;
}

template <typename Predicate>
inline std::vector<const WeaponCatalogDef*> collectBotCatalogCandidates(Competitive::FpsTeam team,
                                                                          int32_t max_price,
                                                                          Predicate predicate) {
    std::vector<const WeaponCatalogDef*> candidates;
    if (max_price < 0 || !Competitive::isPlayingTeam(team)) return candidates;
    size_t count = 0;
    const WeaponCatalogDef* defs = weaponCatalog(count);
    for (size_t i = 0; i < count; ++i) {
        const WeaponCatalogDef& def = defs[i];
        if (def.price <= max_price && catalogAvailableToBot(def, team) && predicate(def))
            candidates.push_back(&def);
    }
    // Price is the only approved current purchase signal.  ID resolves equal
    // prices without depending on table traversal or aliases.
    std::sort(candidates.begin(), candidates.end(), [](const WeaponCatalogDef* a, const WeaponCatalogDef* b) {
        return a->price != b->price ? a->price > b->price : a->id < b->id;
    });
    if (candidates.size() > kBotBuyAttemptsPerGroup)
        candidates.resize(kBotBuyAttemptsPerGroup);
    return candidates;
}

inline void appendBotBuyCandidates(BuyPlan& plan, BuyPlanGroup group,
                                   const std::vector<const WeaponCatalogDef*>& candidates) {
    for (const WeaponCatalogDef* candidate : candidates)
        plan.requests.push_back(BuyPlanRequest{candidate->id, group});
}

inline const WeaponCatalogDef* firstBotCatalogCandidate(const std::vector<const WeaponCatalogDef*>& candidates) {
    return candidates.empty() ? nullptr : candidates.front();
}

// Failures caused by a stale/bad catalog row or by a now-lower wallet may use
// the next *different* catalog ID in the same capped group.  All state/phase
// failures stop that group immediately; retrying them would create a request
// loop without changing eligibility.
inline bool shouldTryNextBotBuyCandidate(Competitive::BuyResult result) {
    using R = Competitive::BuyResult;
    return result == R::UnknownItem || result == R::InsufficientFunds || result == R::WrongTeam;
}

inline BuyPlan selectBuyPlan(int32_t money, Competitive::FpsTeam team, uint8_t difficulty, uint8_t loss_streak) {
    BuyPlan plan;
    if (money <= 0 || !Competitive::isPlayingTeam(team)) return plan;
    const uint8_t d = clampDifficulty(difficulty);
    const uint8_t team_bit = catalogTeamBit(team);

    const auto primary = [&](int32_t ceiling) {
        return collectBotCatalogCandidates(team, ceiling, [](const WeaponCatalogDef& def) {
            return def.kind == BuyItemKind::Weapon && def.slot == WeaponSlot::Primary;
        });
    };
    const auto sidearm = [&](int32_t ceiling) {
        return collectBotCatalogCandidates(team, ceiling, [](const WeaponCatalogDef& def) {
            return def.kind == BuyItemKind::Weapon && def.slot == WeaponSlot::Secondary;
        });
    };
    const auto universal_equipment = [&](int32_t ceiling) {
        return collectBotCatalogCandidates(team, ceiling, [](const WeaponCatalogDef& def) {
            return def.kind == BuyItemKind::Equipment && def.team_mask == kCatalogTeamBoth;
        });
    };
    const auto team_equipment = [&](int32_t ceiling) {
        return collectBotCatalogCandidates(team, ceiling, [team_bit](const WeaponCatalogDef& def) {
            return def.kind == BuyItemKind::Equipment && def.team_mask == team_bit;
        });
    };
    const auto grenade = [&](int32_t ceiling) {
        return collectBotCatalogCandidates(team, ceiling, [](const WeaponCatalogDef& def) {
            return def.kind == BuyItemKind::Grenade;
        });
    };

    // Full buys reserve the best catalog equipment for both sides, any
    // side-specific equipment (currently the defender kit), and a catalog
    // grenade for the highest difficulty.  The remaining wallet defines the
    // legal primary alternatives; no duplicated money thresholds are needed.
    const auto full_armor = universal_equipment(money);
    const WeaponCatalogDef* armor = firstBotCatalogCandidate(full_armor);
    const auto full_team_equipment = team_equipment(money);
    const WeaponCatalogDef* team_item = firstBotCatalogCandidate(full_team_equipment);
    const auto full_grenade = d >= 3 ? grenade(money) : std::vector<const WeaponCatalogDef*>{};
    const WeaponCatalogDef* grenade_item = firstBotCatalogCandidate(full_grenade);
    const int32_t full_reserve = (armor ? armor->price : 0) + (team_item ? team_item->price : 0) +
                                 (grenade_item ? grenade_item->price : 0);
    const auto full_primary = primary(money - full_reserve);
    if (!full_primary.empty()) {
        plan.kind = BuyPlanKind::Full;
        appendBotBuyCandidates(plan, BuyPlanGroup::Primary, full_primary);
        if (armor) appendBotBuyCandidates(plan, BuyPlanGroup::Armor, full_armor);
        if (team_item) appendBotBuyCandidates(plan, BuyPlanGroup::TeamEquipment, full_team_equipment);
        if (grenade_item) appendBotBuyCandidates(plan, BuyPlanGroup::Grenade, full_grenade);
        return plan;
    }

    // A force buy reserves the least expensive cross-team equipment row and
    // then selects from the primary rows the current catalog can actually pay
    // for.  This is intentionally derived from catalog contents, so a roster
    // change cannot leave a stale hard-coded SMG threshold behind.
    const auto force_armor = universal_equipment(money);
    const WeaponCatalogDef* force_armor_item = force_armor.empty() ? nullptr : force_armor.back();
    const auto force_primary = primary(money - (force_armor_item ? force_armor_item->price : 0));
    if (!force_primary.empty()) {
        plan.kind = BuyPlanKind::Force;
        appendBotBuyCandidates(plan, BuyPlanGroup::Primary, force_primary);
        if (force_armor_item) {
            std::vector<const WeaponCatalogDef*> one_armor{force_armor_item};
            appendBotBuyCandidates(plan, BuyPlanGroup::Armor, one_armor);
        }
        return plan;
    }

    // A loss-streak eco is only selected when the current catalog cannot
    // assemble a force bundle. Its sidearm choice and affordability therefore
    // come entirely from the same catalog as the server buy boundary.
    if (loss_streak >= 2) {
        const auto eco_sidearm = sidearm(money);
        if (!eco_sidearm.empty()) {
            plan.kind = BuyPlanKind::Eco;
            appendBotBuyCandidates(plan, BuyPlanGroup::Sidearm, eco_sidearm);
            return plan;
        }
    }

    // Pistol-round policy contains no weapon alias: higher difficulties may
    // buy the least expensive legal universal equipment if they can afford it.
    if (d >= 2 && !force_armor.empty()) {
        const WeaponCatalogDef* pistol_armor = force_armor.back();
        if (pistol_armor && pistol_armor->price <= money) {
            plan.kind = BuyPlanKind::Pistol;
            std::vector<const WeaponCatalogDef*> one_armor{pistol_armor};
            appendBotBuyCandidates(plan, BuyPlanGroup::Armor, one_armor);
            return plan;
        }
    }
    plan.kind = BuyPlanKind::Pistol;
    return plan;
}

// ---- Input synthesis: same Net::InputSample humans send ----
inline float turnTowards(float cur, float target, float max_step) {
    float d = target - cur; const float pi = 3.14159265358979323846f;
    while (d > pi) d -= 2 * pi; while (d < -pi) d += 2 * pi;
    return cur + std::clamp(d, -max_step, max_step);
}
inline Net::InputSample synthesizeInput(BotComponent& b, const NavAgentComponent& nav, const BotIntentComponent& intent, uint32_t tick, float dt) {
    const auto& diff = difficultyFor(b.difficulty);
    Net::InputSample s{}; s.tick = tick;
    float target_yaw = intent.aim_yaw, target_pitch = intent.aim_pitch;
    bool engaging = b.state == BotState::Engage;
    if (engaging) {
        // Aim error perturbs aim only; weapon spread is untouched server-side.
        float err = glm::radians(diff.aim_error_deg);
        target_yaw += rngSigned(b.rng_state) * err; target_pitch += rngSigned(b.rng_state) * err * 0.5f;
    } else if (glm::length(nav.desired_velocity) > 0.1f) {
        target_yaw = std::atan2(nav.desired_velocity.x, nav.desired_velocity.z); target_pitch = 0.0f;
    }
    float max_turn = glm::radians(diff.aim_turn_rate_deg) * std::max(dt, 0.0f);
    b.aim_yaw = turnTowards(b.aim_yaw, target_yaw, max_turn > 0 ? max_turn : 6.28f);
    b.aim_pitch = std::clamp(turnTowards(b.aim_pitch, target_pitch, max_turn > 0 ? max_turn : 6.28f), -1.5f, 1.5f);
    s.camera_yaw = b.aim_yaw; s.camera_pitch = b.aim_pitch;
    glm::vec3 wish = nav.desired_velocity;
    if (intent.want_use) wish = glm::vec3(0); // plant/defuse hold: no movement
    glm::vec3 fwd = forwardFromYaw(b.aim_yaw);
    glm::vec3 right(-fwd.z, 0.0f, fwd.x); // yaw * (-1,0,0)
    // right check: forward (sin,0,cos); yaw*(-1,0,0)=(-cos,0,sin)=(-fwd.z,0,fwd.x)
    float speed = glm::length(glm::vec3(wish.x, 0, wish.z));
    float scale = nav.max_speed > 0 ? std::clamp(speed / nav.max_speed, 0.0f, 1.0f) : 0.0f;
    glm::vec3 dir = speed > 0.001f ? wish / speed : glm::vec3(0);
    s.move_forward = std::clamp(glm::dot(dir, fwd) * scale, -1.0f, 1.0f);
    s.move_right = std::clamp(glm::dot(dir, right) * scale, -1.0f, 1.0f);
    // Engage strafe (bots.md state table): lateral movement while aiming at the
    // visible enemy, direction flipped by rng every strafe_interval seconds.
    // strafe_interval == 0 (Easy) means stand. Direct input only, no teleport.
    if (engaging && intent.want_attack && diff.strafe_interval > 0.0f) {
        b.strafe_timer += std::max(dt, 0.0f);
        if (b.strafe_timer >= diff.strafe_interval) { b.strafe_timer = 0.0f; b.strafe_dir = rngSigned(b.rng_state) >= 0.0f ? (int8_t)1 : (int8_t)-1; }
        s.move_right = 0.8f * (float)b.strafe_dir;
    }
    if (s.move_forward > 0.01f) s.buttons |= Net::InputFlags::MOVE_FORWARD; else if (s.move_forward < -0.01f) s.buttons |= Net::InputFlags::MOVE_BACK;
    if (s.move_right > 0.01f) s.buttons |= Net::InputFlags::MOVE_RIGHT; else if (s.move_right < -0.01f) s.buttons |= Net::InputFlags::MOVE_LEFT;
    if (intent.want_use) s.buttons |= Net::InputFlags::USE;
    if (intent.want_attack && engaging) s.buttons |= Net::InputFlags::ATTACK;
    else if (intent.want_attack && b.state == BotState::Engage) s.buttons |= Net::InputFlags::ATTACK;
    if (intent.want_jump) s.buttons |= Net::InputFlags::JUMP;
    if (intent.want_crouch) s.buttons |= Net::InputFlags::CROUCH;
    if (intent.want_weapon_slot) s.buttons = Net::withWeaponSelect(s.buttons, intent.want_weapon_slot);
    return s;
}

} // namespace Bots
