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

// Candidate eligibility is deliberately value-only. The server is the sole
// caller that turns a qualifying ECS row into an EnemyCandidate with a
// transform/raycast; decisions receive only the later percept summary.
inline bool isOpposingLivingPlayer(Competitive::FpsTeam observer_team,
                                   Competitive::FpsTeam candidate_team,
                                   bool candidate_is_player, bool candidate_alive)
{
    return candidate_is_player && candidate_alive &&
        Competitive::isPlayingTeam(observer_team) &&
        Competitive::isPlayingTeam(candidate_team) && observer_team != candidate_team;
}

// Bots use an authored tie-break only when filling a competitive roster. This
// gives a bot-first acceptance run a stable Attacker and leaves the normal
// human team-selection policy unchanged. Capacity remains enforced by the
// existing server spawn boundary.
inline Competitive::FpsTeam selectCompetitiveBotJoinTeam(int32_t attackers, int32_t defenders,
                                                         int32_t team_size)
{
    const bool attackers_open = attackers < team_size;
    const bool defenders_open = defenders < team_size;
    if (!attackers_open && !defenders_open)
        return Competitive::FpsTeam::None;
    if (!attackers_open)
        return Competitive::FpsTeam::Defenders;
    if (!defenders_open)
        return Competitive::FpsTeam::Attackers;
    return attackers <= defenders ? Competitive::FpsTeam::Attackers
                                  : Competitive::FpsTeam::Defenders;
}

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

// An admitted navigation request has one terminal classification.  The detail
// deliberately refines LocalBlock only; it never becomes a competing reason.
enum class NavFailureCategory : uint8_t {
    None,
    StartOffMesh,
    GoalOffMesh,
    NoPath,
    QueryCapacity,
    LocalBlock,
    Stuck,
};

enum class LocalBlockDetail : uint8_t { None, Static, Crowd };

inline const char* navFailureCategoryName(NavFailureCategory category)
{
    switch (category) {
    case NavFailureCategory::None: return "none";
    case NavFailureCategory::StartOffMesh: return "start_off_mesh";
    case NavFailureCategory::GoalOffMesh: return "goal_off_mesh";
    case NavFailureCategory::NoPath: return "no_path";
    case NavFailureCategory::QueryCapacity: return "query_capacity";
    case NavFailureCategory::LocalBlock: return "local_block";
    case NavFailureCategory::Stuck: return "stuck";
    }
    return "unknown";
}

inline const char* localBlockDetailName(LocalBlockDetail detail)
{
    switch (detail) {
    case LocalBlockDetail::None: return "none";
    case LocalBlockDetail::Static: return "static";
    case LocalBlockDetail::Crowd: return "crowd";
    }
    return "unknown";
}

struct NavAgentComponent {
    float max_speed = 5.0f, max_accel = 20.0f, arrive_radius = 1.2f, waypoint_radius = 0.6f, repath_cooldown = 0.25f;
    std::vector<glm::vec3> path; uint32_t path_index = 0;
    glm::vec3 goal{0}; bool has_goal = false; bool path_valid = false;
    float repath_timer = 0; glm::vec3 desired_velocity{0}; bool stuck = false;
    int32_t path_failures = 0;
    float route_age = 0.0f;
    float local_block_time = 0.0f;
    uint16_t local_block_ticks = 0;
    LocalBlockDetail local_block_detail = LocalBlockDetail::None;
    uint32_t recovery_count = 0;
    NavFailureCategory last_failure = NavFailureCategory::None;
    bool local_block_confirmed = false;
    bool capacity_failure_active = false;
    // AI-N2 doorway state belongs to the affected pawn. It is only a bounded
    // local wait over the existing Crowd probe; it owns neither a route nor a
    // physics object and never grants passage.
    uint32_t route_generation = 0;
    uint32_t doorway_route_generation = 0;
    uint32_t doorway_path_index = 0;
    uint16_t doorway_crowd_ticks = 0;
    bool doorway_yielding = false;
    // A request is owned by this bot until the world scheduler grants or
    // rejects it.  The tick is deliberately data rather than wall-clock time:
    // replay order remains stable regardless of frame duration.
    bool path_request_pending = false;
    uint32_t path_request_tick = 0;
};
struct SteeringComponent {
    glm::vec3 velocity{0}; float stuck_time = 0; glm::vec3 last_pos{0}; bool has_last = false;
    uint8_t unstuck_phase = 0; float unstuck_timer = 0; float stuck_window_time = 0;
    uint16_t stuck_window_ticks = 0;
    glm::vec3 window_start{0}; float horizontal_command_speed = 0.0f;
};
struct BotComponent {
    uint8_t difficulty = 1; BotState state = BotState::Idle; BotState resume_state = BotState::Advance; bool has_resume = false;
    float state_time = 0, next_decision_time = 0, engage_lost_time = 0, aim_yaw = 0, aim_pitch = 0;
    float strafe_timer = 0; int8_t strafe_dir = 1; // Engage strafe pattern (bots.md: flips every strafe_interval)
    uint32_t rng_state = 1; entt::entity pawn = entt::null; int32_t bot_index = 0;
    bool buy_done = false;
};
struct BotPerceptComponent {
    entt::entity visible_enemy = entt::null; glm::vec3 last_seen_pos{0}; glm::vec3 last_seen_aim_point{0}; float last_seen_age = 1e9f;
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
// blocked(target, origin, point) -> true when a weapon trace reaches an
// obstruction before the target point.  The target identity is required so a
// target-owned physics body is not mistaken for level geometry.
struct EnemyCandidate { entt::entity entity = entt::null; glm::vec3 position{0}; };
using BlockedFn = std::function<bool(entt::entity, const glm::vec3&, const glm::vec3&)>;
inline constexpr float kBotWeaponOriginHeight = 0.62f;
inline constexpr float kBotWeaponAimHeight = 1.35f;
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
    p.last_seen_aim_point = glm::vec3(0.0f);
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
    entt::entity best = entt::null; float best_frac = 0; glm::vec3 best_pos{0}; glm::vec3 best_aim_point{0};
    glm::vec3 fwd = forwardFromYaw(yaw);
    for (const auto& e : enemies) {
        if (e.entity == entt::null || !std::isfinite(e.position.x) || !std::isfinite(e.position.y) || !std::isfinite(e.position.z)) continue;
        const glm::vec3 aim_point = e.position + glm::vec3(0.0f, kBotWeaponAimHeight, 0.0f);
        glm::vec3 to = aim_point - eye; float dist = glm::length(to);
        if (dist > d.view_distance || dist < 0.001f) continue;
        glm::vec3 flat(to.x, 0, to.z); float flat_len = glm::length(flat);
        if (flat_len > 0.001f) {
            float cosang = glm::dot(fwd, flat / flat_len);
            float ang = std::acos(std::clamp(cosang, -1.0f, 1.0f));
            if (ang > glm::radians(d.fov_deg) * 0.5f && dist > 3.0f) continue;
        }
        // The engagement point must be clear on the same line the server will
        // later use for the bot's weapon trace.  Peripheral capsule samples
        // may refine sight coverage, but can never authorize a shot through a
        // divider that blocks this canonical line.
        if (blocked && blocked(e.entity, eye, aim_point)) continue;
        int vis = 0;
        const float offs[3] = {0.3f, kBotWeaponAimHeight, 1.65f};
        for (float o : offs) {
            const glm::vec3 pt = e.position + glm::vec3(0.0f, o, 0.0f);
            if (!blocked || !blocked(e.entity, eye, pt)) ++vis;
        }
        float frac = vis / 3.0f;
        // Candidate collection can come from an ECS view. Preserve replayable
        // selection when two visible candidates have the same coverage.
        if (frac > best_frac ||
            (frac == best_frac && frac > 0.0f &&
             (best == entt::null || entt::to_integral(e.entity) < entt::to_integral(best)))) {
            best_frac = frac;
            best = e.entity;
            best_pos = e.position;
            best_aim_point = aim_point;
        }
    }
    p.visible_fraction = best_frac;
    if (best_frac > 0.0f) {
        if (p.visible_enemy != best) p.confirm_timer = 0.0f;
        p.visible_enemy = best;
        p.last_seen_pos = best_pos;
        p.last_seen_aim_point = best_aim_point;
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
    ++n.route_generation;
    n.doorway_route_generation = 0;
    n.doorway_path_index = 0;
    n.doorway_crowd_ticks = 0;
    n.doorway_yielding = false;
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
    n.route_age = 0.0f;
    ++n.route_generation;
    n.doorway_route_generation = 0;
    n.doorway_path_index = 0;
    n.doorway_crowd_ticks = 0;
    n.doorway_yielding = false;
}

inline void acceptPathRequest(NavAgentComponent& n, const std::vector<glm::vec3>& points,
                              const glm::vec3& resolved_goal, SteeringComponent& s) {
    setPath(n, points, resolved_goal);
    n.repath_timer = n.repath_cooldown;
    n.stuck = false;
    n.route_age = 0.0f;
    n.local_block_time = 0.0f;
    n.local_block_ticks = 0;
    n.local_block_detail = LocalBlockDetail::None;
    n.local_block_confirmed = false;
    n.last_failure = NavFailureCategory::None;
    n.capacity_failure_active = false;
    s.stuck_time = 0.0f;
    s.stuck_window_time = 0.0f;
    s.stuck_window_ticks = 0;
    s.window_start = s.last_pos;
    s.horizontal_command_speed = 0.0f;
}

inline constexpr float kBotLocalBlockConfirmSeconds = 0.75f;
inline constexpr uint16_t kBotLocalBlockConfirmTicks = 45u;
inline constexpr float kBotStuckCommandSpeed = 1.0f;

// AI-N2 is deliberately a local, template-owned velocity adjustment. The
// server creates these value samples only after filtering generic character
// overlaps. Nothing below retains an entity, player, team, route, or target.
inline constexpr float kBotAvoidanceQueryRadius = 2.40f;
inline constexpr uint32_t kBotAvoidanceSampleCap = 8u;
// Physics queries require a caller-owned output array.  Keep a bounded
// collection buffer larger than the contract sample cap so the server can
// order all nearby returned candidates by stable actor key before selecting
// the eight value samples consumed by steering.
inline constexpr uint32_t kBotAvoidanceCollectionCap = 32u;
inline constexpr float kBotAvoidanceClearance = 0.90f;
inline constexpr float kBotAvoidanceMaxContribution = 2.00f;
inline constexpr uint16_t kBotDoorwayPriorityTicks = 15u;

struct LocalActorCandidate {
    glm::vec3 horizontal_offset{0.0f};
    bool human_present = false;
    int32_t bot_index = 0;
    uint16_t owner_client_id = 0;
};

// The server owns these transient keys only long enough to make overlap order
// deterministic. Conversion immediately strips them before planning movement.
inline bool localActorCandidateEarlier(const LocalActorCandidate& a, const LocalActorCandidate& b)
{
    if (a.human_present != b.human_present) return a.human_present;
    if (a.human_present) return a.owner_client_id < b.owner_client_id;
    if (a.bot_index != b.bot_index) return a.bot_index < b.bot_index;
    return a.owner_client_id < b.owner_client_id;
}

struct LocalAvoidanceSample {
    glm::vec3 horizontal_offset{0.0f};
    bool has_doorway_priority = false;
};

inline std::vector<LocalActorCandidate> sortAndCapLocalActorCandidates(
    std::vector<LocalActorCandidate> candidates)
{
    std::sort(candidates.begin(), candidates.end(), localActorCandidateEarlier);
    if (candidates.size() > kBotAvoidanceSampleCap)
        candidates.resize(kBotAvoidanceSampleCap);
    return candidates;
}

inline std::vector<LocalAvoidanceSample> makeLocalAvoidanceSamples(
    const std::vector<LocalActorCandidate>& candidates,
    int32_t self_bot_index, uint16_t self_owner_client_id)
{
    std::vector<LocalAvoidanceSample> samples;
    samples.reserve(candidates.size());
    for (const LocalActorCandidate& candidate : candidates) {
        const bool priority = candidate.human_present ||
            candidate.bot_index < self_bot_index ||
            (candidate.bot_index == self_bot_index &&
             candidate.owner_client_id < self_owner_client_id);
        samples.push_back({candidate.horizontal_offset, priority});
    }
    return samples;
}

struct LocalAvoidancePlan {
    glm::vec3 desired_velocity{0.0f};
    glm::vec3 contribution{0.0f};
    bool lower_doorway_priority = false;
};

inline LocalAvoidancePlan planLocalAvoidance(const glm::vec3& desired_velocity,
                                              const std::vector<LocalAvoidanceSample>& samples,
                                              float max_speed)
{
    LocalAvoidancePlan plan;
    glm::vec3 desired = desired_velocity;
    desired.y = 0.0f;
    glm::vec3 sum(0.0f);
    for (const LocalAvoidanceSample& sample : samples) {
        glm::vec3 offset = sample.horizontal_offset;
        offset.y = 0.0f;
        const float distance = glm::length(offset);
        if (distance >= kBotAvoidanceClearance || distance < 0.01f)
            continue;
        const float weight = (kBotAvoidanceClearance - distance) / kBotAvoidanceClearance;
        sum -= (offset / distance) * (kBotAvoidanceMaxContribution * weight);
    }
    const float sum_length = glm::length(sum);
    if (sum_length > kBotAvoidanceMaxContribution)
        sum *= kBotAvoidanceMaxContribution / sum_length;
    const float merged_length = glm::length(desired + sum);
    plan.desired_velocity = desired + sum;
    if (merged_length > std::max(max_speed, 0.0f) && merged_length > 0.0f)
        plan.desired_velocity *= std::max(max_speed, 0.0f) / merged_length;
    plan.contribution = sum;
    for (const LocalAvoidanceSample& sample : samples)
        plan.lower_doorway_priority = plan.lower_doorway_priority || sample.has_doorway_priority;
    return plan;
}

inline void clearDoorwayWait(NavAgentComponent& n)
{
    n.doorway_route_generation = 0;
    n.doorway_path_index = 0;
    n.doorway_crowd_ticks = 0;
    n.doorway_yielding = false;
}

// A changed route/waypoint starts a new continuous crowd interval. Priority
// is deliberately recomputed by the caller each fixed tick from local samples.
inline bool advanceDoorwayWait(NavAgentComponent& n, bool continuous_crowd,
                               bool separation_eligible, bool lower_priority)
{
    if (!continuous_crowd || !separation_eligible) {
        clearDoorwayWait(n);
        return false;
    }
    if (n.doorway_route_generation != n.route_generation ||
        n.doorway_path_index != n.path_index) {
        n.doorway_route_generation = n.route_generation;
        n.doorway_path_index = n.path_index;
        n.doorway_crowd_ticks = 1u;
        n.doorway_yielding = false;
        return false;
    }
    if (n.doorway_crowd_ticks < kBotLocalBlockConfirmTicks)
        ++n.doorway_crowd_ticks;
    n.doorway_yielding = lower_priority &&
        n.doorway_crowd_ticks >= kBotDoorwayPriorityTicks &&
        n.doorway_crowd_ticks < kBotLocalBlockConfirmTicks;
    return n.doorway_yielding;
}

// The probe itself stays server-owned because it needs generic physics.  This
// value helper only tracks persistence of an already-redacted result, so it is
// deterministic and does not retain a hit point or entity identity.
inline bool observeLocalBlock(NavAgentComponent& n, LocalBlockDetail observed, float dt)
{
    const float elapsed = std::max(dt, 0.0f);
    if (observed == LocalBlockDetail::None) {
        n.local_block_time = 0.0f;
        n.local_block_ticks = 0;
        n.local_block_detail = LocalBlockDetail::None;
        n.local_block_confirmed = false;
        return false;
    }
    if (n.local_block_detail != observed) {
        n.local_block_detail = observed;
        n.local_block_time = elapsed;
        n.local_block_ticks = elapsed > 0.0f ? 1u : 0u;
        n.local_block_confirmed = false;
        return false;
    }
    n.local_block_time += elapsed;
    if (elapsed > 0.0f && n.local_block_ticks < kBotLocalBlockConfirmTicks)
        ++n.local_block_ticks;
    // The server invokes this on its 60 Hz fixed tick.  Keep the elapsed
    // value for diagnostics, but use the equivalent tick count for the
    // recovery threshold so 45 x (1 / 60) confirms exactly once despite
    // binary floating-point rounding.
    if (!n.local_block_confirmed &&
        (n.local_block_ticks >= kBotLocalBlockConfirmTicks ||
         n.local_block_time + 0.00001f >= kBotLocalBlockConfirmSeconds)) {
        n.local_block_confirmed = true;
        return true;
    }
    return false;
}

inline void recoverNavigation(NavAgentComponent& n, SteeringComponent& s,
                              NavFailureCategory category)
{
    // Recovery deliberately reuses the normal failed-request backoff.  It
    // clears movement state only; the caller's next intent queues a fresh
    // Detour request instead of writing a transform, velocity, or body.
    rejectPathRequest(n);
    n.stuck = false;
    n.last_failure = category;
    ++n.recovery_count;
    n.local_block_time = 0.0f;
    n.local_block_ticks = 0;
    n.local_block_detail = LocalBlockDetail::None;
    n.local_block_confirmed = false;
    s.velocity = glm::vec3(0.0f);
    s.horizontal_command_speed = 0.0f;
    s.stuck_time = 0.0f;
    s.stuck_window_time = 0.0f;
    s.stuck_window_ticks = 0;
    s.window_start = s.last_pos;
}

struct NavigationRecoveryResult {
    NavFailureCategory category = NavFailureCategory::None;
    LocalBlockDetail detail = LocalBlockDetail::None;
};

inline void updateSteering(NavAgentComponent& n, SteeringComponent& s,
                           const glm::vec3& pos, float dt);

// Shared S4 branch used by tickBots.  The caller owns the physics probe; this
// helper owns the mutually-exclusive LocalBlock/Stuck ordering so a fixture
// cannot accidentally prove a different recovery path than the server uses.
inline NavigationRecoveryResult tickNavigationRecovery(
    NavAgentComponent& n, SteeringComponent& s, const glm::vec3& pos,
    LocalBlockDetail observed_block, float dt, bool suppress_stuck = false)
{
    if (observeLocalBlock(n, observed_block, dt)) {
        recoverNavigation(n, s, NavFailureCategory::LocalBlock);
        return {NavFailureCategory::LocalBlock, observed_block};
    }
    updateSteering(n, s, pos, dt);
    if (suppress_stuck) {
        // A deliberate doorway yield is neutral input, not failed movement.
        // Drop its low-progress window so yielding cannot become AI-N1 Stuck.
        n.stuck = false;
        s.stuck_time = 0.0f;
        s.stuck_window_time = 0.0f;
        s.stuck_window_ticks = 0;
        s.window_start = pos;
        return {};
    }
    if (n.stuck) {
        recoverNavigation(n, s, NavFailureCategory::Stuck);
        return {NavFailureCategory::Stuck, LocalBlockDetail::None};
    }
    return {};
}

struct NavAdmissionClassification {
    NavFailureCategory category = NavFailureCategory::None;
    bool inspect_capacity = false;
    bool capacity_inspection_suppressed = false;
};

// Endpoint order is explicit: a start failure wins when both endpoints are
// off mesh.  A capacity label requires a production capacity signal; a
// cooldown can suppress only the expanded confirmation, never relabel a
// topological NoPath.  With an open gate, inspect_capacity asks the caller to
// run the isolated query and call again with capacity_confirmed set.
inline NavAdmissionClassification classifyNavAdmission(
    bool start_projected, bool goal_projected, bool route_valid,
    bool production_capacity_exhausted, bool capacity_gate_open,
    bool capacity_confirmed)
{
    if (!start_projected)
        return {NavFailureCategory::StartOffMesh};
    if (!goal_projected)
        return {NavFailureCategory::GoalOffMesh};
    if (route_valid)
        return {};
    if (!production_capacity_exhausted)
        return {NavFailureCategory::NoPath};
    if (capacity_confirmed)
        return {NavFailureCategory::QueryCapacity};
    if (!capacity_gate_open)
        return {NavFailureCategory::QueryCapacity, false, true};
    return {NavFailureCategory::NoPath, true, false};
}

inline bool currentWaypointDirection(const NavAgentComponent& n, const glm::vec3& pos,
                                     glm::vec3& out_direction)
{
    if (!n.has_goal || !n.path_valid || n.path.empty() || n.path_index >= n.path.size())
        return false;
    glm::vec3 delta = n.path[n.path_index] - pos;
    delta.y = 0.0f;
    const float length = glm::length(delta);
    if (length <= 0.001f)
        return false;
    out_direction = delta / length;
    return true;
}

inline float localBlockProbeRange(float capsule_radius, const NavAgentComponent& n)
{
    return std::max(2.0f * std::max(capsule_radius, 0.0f) + 0.05f,
                    std::max(n.max_speed, 0.0f) * std::max(n.repath_cooldown, 0.0f));
}

inline void updateSteering(NavAgentComponent& n, SteeringComponent& s,
                           const glm::vec3& pos, float dt) {
    n.desired_velocity = glm::vec3(0);
    if (!s.has_last) { s.last_pos = pos; s.window_start = pos; s.has_last = true; }
    if (!n.has_goal || !n.path_valid || n.path.empty()) { s.velocity = glm::vec3(0); s.horizontal_command_speed = 0.0f; n.route_age = 0.0f; return; }
    n.route_age += std::max(dt, 0.0f);
    while (n.path_index < n.path.size()) {
        glm::vec3 to = n.path[n.path_index] - pos; to.y = 0;
        if (glm::length(to) < n.waypoint_radius) { ++n.path_index; continue; } break;
    }
    if (n.path_index >= n.path.size()) { n.has_goal = false; s.velocity = glm::vec3(0); s.horizontal_command_speed = 0.0f; return; }
    glm::vec3 target = n.path[n.path_index];
    glm::vec3 to = target - pos; to.y = 0; float dist = glm::length(to);
    glm::vec3 final_to = n.goal - pos; final_to.y = 0; float final_dist = glm::length(final_to);
    float arrive = std::clamp(final_dist / std::max(n.arrive_radius, 0.01f), 0.25f, 1.0f);
    glm::vec3 desired = dist > 0.001f ? (to / dist) * n.max_speed * arrive : glm::vec3(0);
    // move_towards smoothing
    glm::vec3 delta = desired - s.velocity; float max_delta = n.max_accel * dt; float len = glm::length(delta);
    s.velocity = len > max_delta && len > 0.0f ? s.velocity + delta / len * max_delta : desired;
    n.desired_velocity = s.velocity;
    s.horizontal_command_speed = glm::length(glm::vec2(n.desired_velocity.x, n.desired_velocity.z));
    // Stuck: commanded >1 m/s but <0.15 m displacement over 0.75 s window.
    const float elapsed = std::max(dt, 0.0f);
    s.stuck_window_time += elapsed;
    if (elapsed > 0.0f && s.stuck_window_ticks < kBotLocalBlockConfirmTicks)
        ++s.stuck_window_ticks;
    if (s.stuck_window_ticks >= kBotLocalBlockConfirmTicks ||
        s.stuck_window_time + 0.00001f >= kBotLocalBlockConfirmSeconds) {
        glm::vec3 disp = pos - s.window_start; disp.y = 0;
        n.stuck = s.horizontal_command_speed > kBotStuckCommandSpeed && glm::length(disp) < 0.15f;
        s.stuck_time = n.stuck ? s.stuck_time + s.stuck_window_time : 0.0f;
        s.window_start = pos; s.stuck_window_time = 0; s.stuck_window_ticks = 0;
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
        if (p.visible_fraction > 0.0f) {
            b.engage_lost_time = 0;
            intent.want_attack = true;
            const glm::vec3 weapon_origin = in.bot_pos + glm::vec3(0.0f, kBotWeaponOriginHeight, 0.0f);
            const glm::vec3 ad = p.last_seen_aim_point - weapon_origin;
            intent.aim_yaw = std::atan2(ad.x, ad.z);
            intent.aim_pitch = std::clamp(std::atan2(ad.y,
                std::max(glm::length(glm::vec3(ad.x, 0.0f, ad.z)), 0.01f)), -1.5f, 1.5f);
        }
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
