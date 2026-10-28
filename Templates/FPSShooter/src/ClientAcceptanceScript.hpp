#pragma once

#include "Network/NetworkInputState.hpp"
#include "shared/CompetitiveLogic.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "Network/NetworkProtocol.hpp"
#include "Utils/Log.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace FPSShooter
{
    // A deliberately narrow, opt-in driver for process acceptance runs. It is
    // inactive unless FPS_TEST_ACTION_SCRIPT names a valid action file.
    enum class ClientAcceptanceActionKind : uint8_t
    {
        Team,
        Buy,
        Input,
        MoveToLive,
        WaitBombPlanted,
        HoldUseLive,
        HoldUseDefuse,
        Wait
    };

    struct ClientAcceptanceObservation
    {
        bool connected = false;
        bool has_round_state = false;
        // ROUND_STATE is received on the existing reliable competitive channel.
        bool bomb_planted = false;
        Competitive::RoundPhase round_phase = Competitive::RoundPhase::WaitingForPlayers;
        Competitive::RoundEndReason last_round_end_reason = Competitive::RoundEndReason::None;
        Competitive::FpsTeam local_team = Competitive::FpsTeam::None;
        bool has_local_pawn = false;
        // Only populated from the replicated/predicted local pawn Transform.
        bool has_local_transform = false;
        float local_position_x = 0.0f;
        float local_position_z = 0.0f;
        // These are latched only when the ordinary local-pawn reconciliation
        // consumes a received WORLD_STATE_UPDATE. They are deliberately
        // distinct from the predicted local Transform used to steer.
        bool has_authoritative_pawn_position = false;
        float authoritative_pawn_position_x = 0.0f;
        float authoritative_pawn_position_z = 0.0f;
        uint64_t authoritative_pawn_snapshot_generation = 0;
        uint32_t last_authoritative_input_tick = 0;
    };

    struct ClientAcceptanceDispatch
    {
        enum class Kind : uint8_t { None, Team, Buy } kind = Kind::None;
        Competitive::FpsTeam team = Competitive::FpsTeam::None;
        bool auto_assign = false;
        uint16_t item_id = 0;
    };

    class ClientAcceptanceScript
    {
    public:
        // Reads FPS_TEST_ACTION_SCRIPT once at module initialization. A missing
        // or invalid file leaves this object inert and never changes gameplay.
        void loadFromEnvironment();
        void reset();

        ClientAcceptanceDispatch tick(float delta_time, const ClientAcceptanceObservation& observation);
        void observeTeamResult(bool accepted, Competitive::FpsTeam assigned);
        void observeBuyResult(uint32_t request_sequence, bool accepted);
        void noteBuyRequestSent(uint32_t request_sequence);
        // Called after an ordinary forward steering command is queued for
        // transport. It never sends an extra acceptance-only message.
        void noteInputCommandSent(uint32_t command_tick, bool steering_move_emitted);
        bool isMoveToLiveSteering() const;

        // Applies the active scripted INPUT_COMMAND intent after the ordinary
        // action sampler has built its state. move_to_live adjusts only the
        // normal command yaw; all other script actions preserve camera fields.
        void applyInput(Net::InputState& state, bool movement_allowed, bool can_fire, bool can_reload) const;

        bool active() const { return m_enabled; }

    private:
        struct Action
        {
            ClientAcceptanceActionKind kind = ClientAcceptanceActionKind::Wait;
            Competitive::FpsTeam team = Competitive::FpsTeam::None;
            bool auto_assign = false;
            uint16_t item_id = 0;
            uint16_t input_buttons = 0;
            float target_x = 0.0f;
            float target_z = 0.0f;
            float tolerance = 0.0f;
            uint32_t duration_ms = 0;
            uint32_t source_line = 0;
        };

        enum class State : uint8_t { Ready, WaitingForTeamResult, WaitingForBuyResult, RunningInput,
            RunningMoveToLive, WaitingForBombPlanted, RunningHoldUseLive, RunningHoldUseDefuse, Waiting };

        bool loadFile(const char* path, std::string& error);
        void disable(const std::string& reason);
        bool canDispatchTeam(const ClientAcceptanceObservation& observation) const;
        bool canDispatchPawnAction(const ClientAcceptanceObservation& observation) const;
        bool canRunLivePawnAction(const ClientAcceptanceObservation& observation) const;
        bool canWaitForBombPlanted(const ClientAcceptanceObservation& observation) const;
        bool moveHasAuthoritativeArrival(const Action& action,
                                          const ClientAcceptanceObservation& observation) const;
        bool atAuthoritativeMoveTarget(const Action& action,
                                       const ClientAcceptanceObservation& observation) const;
        void updateMoveYaw(const Action& action, const ClientAcceptanceObservation& observation);
        void advanceAction();

        bool m_enabled = false;
        std::vector<Action> m_actions;
        size_t m_index = 0;
        State m_state = State::Ready;
        float m_elapsed_seconds = 0.0f;
        float m_move_yaw = 0.0f;
        float m_move_forward = 0.0f;
        uint64_t m_move_start_snapshot_generation = 0;
        // The first ordinary command emitted after the waypoint's steering
        // starts. A later authoritative receipt must acknowledge it before
        // the waypoint may complete.
        uint32_t m_move_steering_input_tick = 0;
        uint32_t m_expected_buy_sequence = 0;
    };
}

namespace FPSShooter::ClientAcceptanceScriptDetail
{
    inline constexpr size_t kMaxActions = 128;
    inline constexpr size_t kMaxLineLength = 256;
    inline constexpr uint32_t kMaxDurationMs = 60000;
    inline constexpr float kMaxMoveCoordinate = 100000.0f;
    inline constexpr float kMaxMoveTolerance = 1000.0f;

    inline std::string trim(std::string text)
    {
        const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
        const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
        return first >= last ? std::string{} : std::string(first, last);
    }

    inline bool parseUnsigned(std::string_view token, uint32_t& out)
    {
        if (token.empty()) return false;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto [ptr, error] = std::from_chars(begin, end, out);
        return error == std::errc{} && ptr == end;
    }

    inline bool parseFiniteFloat(std::string_view token, float& out)
    {
        if (token.empty()) return false;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto [ptr, error] = std::from_chars(begin, end, out);
        return error == std::errc{} && ptr == end && std::isfinite(out);
    }

    inline bool parseTeam(const std::string& token, Competitive::FpsTeam& team, bool& auto_assign)
    {
        auto_assign = false;
        if (token == "auto") { team = Competitive::FpsTeam::Attackers; auto_assign = true; return true; }
        if (token == "attackers") { team = Competitive::FpsTeam::Attackers; return true; }
        if (token == "defenders") { team = Competitive::FpsTeam::Defenders; return true; }
        if (token == "spectator") { team = Competitive::FpsTeam::Spectator; return true; }
        return false;
    }

    inline bool parseInputFlag(const std::string& token, uint16_t& flag)
    {
        using namespace Net::InputFlags;
        if (token == "forward") { flag = MOVE_FORWARD; return true; }
        if (token == "back") { flag = MOVE_BACK; return true; }
        if (token == "left") { flag = MOVE_LEFT; return true; }
        if (token == "right") { flag = MOVE_RIGHT; return true; }
        if (token == "use") { flag = USE; return true; }
        if (token == "fire") { flag = ATTACK; return true; }
        if (token == "jump") { flag = JUMP; return true; }
        if (token == "crouch") { flag = CROUCH; return true; }
        if (token == "sprint") { flag = SPRINT; return true; }
        if (token == "walk") { flag = WALK; return true; }
        if (token == "reload") { flag = RELOAD; return true; }
        if (token == "drop") { flag = DROP; return true; }
        return false;
    }
}

namespace FPSShooter
{
    using namespace ClientAcceptanceScriptDetail;
    inline void ClientAcceptanceScript::loadFromEnvironment()
    {
        reset();
        const char* path = std::getenv("FPS_TEST_ACTION_SCRIPT");
        if (path == nullptr || path[0] == '\0')
            return;

        std::string error;
        if (!loadFile(path, error)) {
            disable(error);
            return;
        }

        m_enabled = true;
        LOG_ENGINE_INFO("[ACCEPT] client action script enabled actions={}", m_actions.size());
    }

    inline void ClientAcceptanceScript::reset()
    {
        m_enabled = false;
        m_actions.clear();
        m_index = 0;
        m_state = State::Ready;
        m_elapsed_seconds = 0.0f;
        m_move_yaw = 0.0f;
        m_move_forward = 0.0f;
        m_move_start_snapshot_generation = 0;
        m_move_steering_input_tick = 0;
        m_expected_buy_sequence = 0;
    }

    inline bool ClientAcceptanceScript::loadFile(const char* path, std::string& error)
    {
        std::ifstream input(path);
        if (!input) {
            error = "client action script disabled: cannot open configured file";
            return false;
        }

        std::string line;
        uint32_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (line.size() > kMaxLineLength) {
                error = "client action script disabled: line " + std::to_string(line_number) + " is too long";
                return false;
            }
            line = trim(std::move(line));
            if (line.empty() || line[0] == '#')
                continue;

            std::istringstream fields(line);
            std::vector<std::string> tokens;
            for (std::string token; fields >> token; )
                tokens.push_back(std::move(token));
            if (tokens.empty())
                continue;
            if (m_actions.size() == kMaxActions) {
                error = "client action script disabled: too many actions";
                return false;
            }

            Action action;
            action.source_line = line_number;
            if (tokens[0] == "team" && tokens.size() == 2) {
                action.kind = ClientAcceptanceActionKind::Team;
                if (!parseTeam(tokens[1], action.team, action.auto_assign)) {
                    error = "client action script disabled: invalid team on line " + std::to_string(line_number);
                    return false;
                }
            } else if (tokens[0] == "buy" && tokens.size() == 2) {
                uint32_t item_id = 0;
                if (!parseUnsigned(tokens[1], item_id) || item_id == 0 || item_id > UINT16_MAX) {
                    error = "client action script disabled: invalid buy item on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::Buy;
                action.item_id = static_cast<uint16_t>(item_id);
            } else if (tokens[0] == "wait" && tokens.size() == 2) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait duration on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::Wait;
            } else if (tokens[0] == "move_to_live" && tokens.size() == 5) {
                if (!parseFiniteFloat(tokens[1], action.target_x) ||
                    !parseFiniteFloat(tokens[2], action.target_z) ||
                    !parseFiniteFloat(tokens[3], action.tolerance) ||
                    !parseUnsigned(tokens[4], action.duration_ms) ||
                    std::abs(action.target_x) > kMaxMoveCoordinate ||
                    std::abs(action.target_z) > kMaxMoveCoordinate ||
                    action.tolerance <= 0.0f || action.tolerance > kMaxMoveTolerance ||
                    action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid move_to_live on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::MoveToLive;
            } else if (tokens[0] == "wait_bomb_planted" && tokens.size() == 2) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait_bomb_planted timeout on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::WaitBombPlanted;
            } else if (tokens[0] == "hold_use_live" && tokens.size() == 2) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid hold_use_live duration on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::HoldUseLive;
            } else if (tokens[0] == "hold_use_defuse" && tokens.size() == 2) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid hold_use_defuse timeout on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::HoldUseDefuse;
            } else if (tokens[0] == "input" && tokens.size() >= 3) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid input duration on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::Input;
                for (size_t index = 2; index < tokens.size(); ++index) {
                    uint16_t flag = 0;
                    if (!parseInputFlag(tokens[index], flag) || (action.input_buttons & flag) != 0) {
                        error = "client action script disabled: invalid input flag on line " + std::to_string(line_number);
                        return false;
                    }
                    action.input_buttons |= flag;
                }
                using namespace Net::InputFlags;
                if ((action.input_buttons & (MOVE_FORWARD | MOVE_BACK)) == (MOVE_FORWARD | MOVE_BACK) ||
                    (action.input_buttons & (MOVE_LEFT | MOVE_RIGHT)) == (MOVE_LEFT | MOVE_RIGHT)) {
                    error = "client action script disabled: conflicting movement on line " + std::to_string(line_number);
                    return false;
                }
            } else {
                error = "client action script disabled: invalid grammar on line " + std::to_string(line_number);
                return false;
            }
            m_actions.push_back(action);
        }

        if (m_actions.empty()) {
            error = "client action script disabled: no actions";
            return false;
        }
        return true;
    }

    inline void ClientAcceptanceScript::disable(const std::string& reason)
    {
        reset();
        LOG_ENGINE_WARN("[ACCEPT] {}", reason);
    }

    inline bool ClientAcceptanceScript::canDispatchTeam(const ClientAcceptanceObservation& observation) const
    {
        return observation.connected && observation.has_round_state &&
            observation.local_team == Competitive::FpsTeam::None && !observation.has_local_pawn;
    }

    inline bool ClientAcceptanceScript::canDispatchPawnAction(const ClientAcceptanceObservation& observation) const
    {
        return observation.connected && observation.has_round_state &&
            Competitive::isPlayingTeam(observation.local_team) && observation.has_local_pawn;
    }

    inline bool ClientAcceptanceScript::canRunLivePawnAction(const ClientAcceptanceObservation& observation) const
    {
        return canDispatchPawnAction(observation) && observation.has_local_transform &&
            observation.round_phase == Competitive::RoundPhase::Live;
    }

    inline bool ClientAcceptanceScript::canWaitForBombPlanted(const ClientAcceptanceObservation& observation) const
    {
        return canDispatchPawnAction(observation) &&
            observation.round_phase == Competitive::RoundPhase::Live;
    }

    inline bool ClientAcceptanceScript::moveHasAuthoritativeArrival(
        const Action& action, const ClientAcceptanceObservation& observation) const
    {
        if (!observation.has_authoritative_pawn_position ||
            m_move_steering_input_tick == 0 ||
            observation.authoritative_pawn_snapshot_generation <= m_move_start_snapshot_generation ||
            static_cast<int32_t>(observation.last_authoritative_input_tick -
                m_move_steering_input_tick) < 0) {
            return false;
        }
        return atAuthoritativeMoveTarget(action, observation);
    }

    inline bool ClientAcceptanceScript::atAuthoritativeMoveTarget(
        const Action& action, const ClientAcceptanceObservation& observation) const
    {
        const float dx = action.target_x - observation.authoritative_pawn_position_x;
        const float dz = action.target_z - observation.authoritative_pawn_position_z;
        return dx * dx + dz * dz <= action.tolerance * action.tolerance;
    }

    inline void ClientAcceptanceScript::updateMoveYaw(
        const Action& action, const ClientAcceptanceObservation& observation)
    {
        // Prefer a fresh reconciliation position so steering follows the
        // server pawn. Before the first received snapshot, use presentation
        // prediction only to produce an ordinary forward/yaw input.
        const bool has_fresh_authority = observation.has_authoritative_pawn_position &&
            observation.authoritative_pawn_snapshot_generation > m_move_start_snapshot_generation;
        const float current_x = has_fresh_authority ? observation.authoritative_pawn_position_x :
            observation.local_position_x;
        const float current_z = has_fresh_authority ? observation.authoritative_pawn_position_z :
            observation.local_position_z;
        const float delta_x = action.target_x - current_x;
        const float delta_z = action.target_z - current_z;
        m_move_yaw = std::atan2(delta_x, delta_z);

        // Reconciliation snapshots are intentionally delayed from the input
        // stream. Full forward thrust with a yaw that chases such a snapshot
        // has enough inertia to orbit a waypoint. Scale the ordinary forward
        // axis inside an eight-unit approach radius so the server pawn brakes
        // before the next reconciliation update. Keep input neutral once the
        // current steering source is already inside the requested tolerance;
        // completion still requires the later authoritative receipt below.
        constexpr float kMoveApproachDistance = 8.0f;
        const float distance = std::sqrt(delta_x * delta_x + delta_z * delta_z);
        if (distance <= action.tolerance) {
            m_move_forward = 0.0f;
        } else {
            m_move_forward = std::clamp(distance / kMoveApproachDistance, 0.12f, 1.0f);
        }
    }

    inline void ClientAcceptanceScript::advanceAction()
    {
        ++m_index;
        m_state = State::Ready;
        m_elapsed_seconds = 0.0f;
        m_move_yaw = 0.0f;
        m_move_forward = 0.0f;
        m_move_start_snapshot_generation = 0;
        m_move_steering_input_tick = 0;
        m_expected_buy_sequence = 0;
        if (m_index == m_actions.size()) {
            m_enabled = false;
            LOG_ENGINE_INFO("[ACCEPT] client action script complete");
        }
    }

    inline ClientAcceptanceDispatch ClientAcceptanceScript::tick(float delta_time, const ClientAcceptanceObservation& observation)
    {
        ClientAcceptanceDispatch dispatch;
        if (!m_enabled || m_index >= m_actions.size())
            return dispatch;

        Action& action = m_actions[m_index];
        switch (m_state) {
        case State::Ready:
            if (action.kind == ClientAcceptanceActionKind::Team) {
                if (!canDispatchTeam(observation)) return dispatch;
                m_state = State::WaitingForTeamResult;
                dispatch.kind = ClientAcceptanceDispatch::Kind::Team;
                dispatch.team = action.team;
                dispatch.auto_assign = action.auto_assign;
                LOG_ENGINE_INFO("[ACCEPT] team select sent line={} team={} auto={}", action.source_line,
                    static_cast<int>(action.team), action.auto_assign ? 1 : 0);
            } else if (action.kind == ClientAcceptanceActionKind::Buy) {
                const bool buy_phase = observation.round_phase == Competitive::RoundPhase::Warmup ||
                    observation.round_phase == Competitive::RoundPhase::Freeze ||
                    observation.round_phase == Competitive::RoundPhase::Live;
                if (!canDispatchPawnAction(observation) || !buy_phase) return dispatch;
                m_state = State::WaitingForBuyResult;
                dispatch.kind = ClientAcceptanceDispatch::Kind::Buy;
                dispatch.item_id = action.item_id;
                LOG_ENGINE_INFO("[ACCEPT] buy request sent line={} item={}", action.source_line, action.item_id);
            } else if (action.kind == ClientAcceptanceActionKind::Input) {
                if (!canDispatchPawnAction(observation)) return dispatch;
                m_state = State::RunningInput;
                m_elapsed_seconds = 0.0f;
                LOG_ENGINE_INFO("[ACCEPT] input begin line={} duration_ms={} buttons={}", action.source_line,
                    action.duration_ms, action.input_buttons);
            } else if (action.kind == ClientAcceptanceActionKind::MoveToLive) {
                if (!canRunLivePawnAction(observation)) return dispatch;
                // Do not accept a target-looking predicted transform or a
                // spawn snapshot already present at action start. Completion
                // requires a later reconciliation snapshot and a receipt for
                // the normal movement command below.
                m_state = State::RunningMoveToLive;
                m_elapsed_seconds = 0.0f;
                m_move_start_snapshot_generation = observation.authoritative_pawn_snapshot_generation;
                m_move_steering_input_tick = 0;
                updateMoveYaw(action, observation);
                LOG_ENGINE_INFO("[ACCEPT] move_to_live begin line={} x={} z={} tolerance={} timeout_ms={}",
                    action.source_line, action.target_x, action.target_z, action.tolerance, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::WaitBombPlanted) {
                if (!canWaitForBombPlanted(observation)) return dispatch;
                m_state = State::WaitingForBombPlanted;
                m_elapsed_seconds = 0.0f;
                LOG_ENGINE_INFO("[ACCEPT] wait_bomb_planted begin line={} timeout_ms={}",
                    action.source_line, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::HoldUseLive) {
                if (!canRunLivePawnAction(observation)) return dispatch;
                m_state = State::RunningHoldUseLive;
                m_elapsed_seconds = 0.0f;
                LOG_ENGINE_INFO("[ACCEPT] hold_use_live begin line={} duration_ms={}", action.source_line, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::HoldUseDefuse) {
                if (!canRunLivePawnAction(observation) || !observation.bomb_planted) return dispatch;
                m_state = State::RunningHoldUseDefuse;
                m_elapsed_seconds = 0.0f;
                LOG_ENGINE_INFO("[ACCEPT] hold_use_defuse begin line={} timeout_ms={}", action.source_line, action.duration_ms);
            } else {
                if (!observation.connected || !observation.has_round_state) return dispatch;
                m_state = State::Waiting;
                m_elapsed_seconds = 0.0f;
            }
            return dispatch;
        case State::RunningInput:
        case State::Waiting:
            if (!observation.connected || !observation.has_round_state) return dispatch;
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f < static_cast<float>(action.duration_ms)) return dispatch;
            LOG_ENGINE_INFO("[ACCEPT] {} complete line={}",
                m_state == State::RunningInput ? "input" : "wait", action.source_line);
            advanceAction();
            return dispatch;
        case State::RunningMoveToLive:
            if (!canRunLivePawnAction(observation)) {
                disable("client action script stopped: move_to_live lost authoritative Live pawn state on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (moveHasAuthoritativeArrival(action, observation)) {
                LOG_ENGINE_INFO("[ACCEPT] move_to_live arrived line={} authoritative_snapshot={} acknowledged_input={}",
                    action.source_line, observation.authoritative_pawn_snapshot_generation,
                    observation.last_authoritative_input_tick);
                advanceAction();
                return dispatch;
            }
            // Keep emitting ordinary forward input while either the receipt or
            // the reconciliation position is still short of the waypoint.
            updateMoveYaw(action, observation);
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: move_to_live timed out on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::WaitingForBombPlanted:
            if (!canWaitForBombPlanted(observation)) {
                disable("client action script stopped: wait_bomb_planted lost authoritative Live round state on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (observation.bomb_planted) {
                LOG_ENGINE_INFO("[ACCEPT] wait_bomb_planted observed authoritative round state line={}", action.source_line);
                advanceAction();
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: wait_bomb_planted timed out on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::RunningHoldUseLive:
            if (!canRunLivePawnAction(observation)) {
                disable("client action script stopped: hold_use_live lost authoritative Live pawn state on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                LOG_ENGINE_INFO("[ACCEPT] hold_use_live complete line={}", action.source_line);
                advanceAction();
            }
            return dispatch;
        case State::RunningHoldUseDefuse:
            // This is deliberately narrower than hold_use_live: a defuse
            // action succeeds only when the existing reliable ROUND_STATE
            // reports the server's BombDefused outcome.  Other Live exits,
            // including death, disconnect, detonation, and elimination, stay
            // failures instead of being silently accepted.
            if (observation.has_round_state &&
                observation.round_phase == Competitive::RoundPhase::RoundEnd &&
                observation.last_round_end_reason == Competitive::RoundEndReason::BombDefused) {
                LOG_ENGINE_INFO("[ACCEPT] hold_use_defuse observed authoritative BombDefused round end line={}",
                    action.source_line);
                advanceAction();
                return dispatch;
            }
            if (!canRunLivePawnAction(observation)) {
                disable("client action script stopped: hold_use_defuse lost Live state without authoritative BombDefused on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: hold_use_defuse timed out awaiting authoritative BombDefused on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::WaitingForTeamResult:
        case State::WaitingForBuyResult:
            return dispatch;
        }
        return dispatch;
    }

    inline void ClientAcceptanceScript::observeTeamResult(bool accepted, Competitive::FpsTeam assigned)
    {
        if (!m_enabled || m_index >= m_actions.size() || m_state != State::WaitingForTeamResult)
            return;
        const Action& action = m_actions[m_index];
        if (!accepted) {
            disable("client action script stopped: team select rejected on line " + std::to_string(action.source_line));
            return;
        }
        LOG_ENGINE_INFO("[ACCEPT] team select accepted line={} team={}", action.source_line, static_cast<int>(assigned));
        advanceAction();
    }

    inline void ClientAcceptanceScript::noteBuyRequestSent(uint32_t request_sequence)
    {
        if (m_enabled && m_index < m_actions.size() && m_state == State::WaitingForBuyResult)
            m_expected_buy_sequence = request_sequence;
    }

    inline bool ClientAcceptanceScript::isMoveToLiveSteering() const
    {
        return m_state == State::RunningMoveToLive;
    }

    inline void ClientAcceptanceScript::noteInputCommandSent(uint32_t command_tick,
                                                               bool steering_move_emitted)
    {
        if (m_enabled && m_index < m_actions.size() && m_state == State::RunningMoveToLive &&
            steering_move_emitted && m_move_steering_input_tick == 0 && command_tick != 0)
            m_move_steering_input_tick = command_tick;
    }

    inline void ClientAcceptanceScript::observeBuyResult(uint32_t request_sequence, bool accepted)
    {
        if (!m_enabled || m_index >= m_actions.size() || m_state != State::WaitingForBuyResult ||
            m_expected_buy_sequence == 0 || request_sequence != m_expected_buy_sequence)
            return;
        const Action& action = m_actions[m_index];
        if (!accepted) {
            disable("client action script stopped: buy request rejected on line " + std::to_string(action.source_line));
            return;
        }
        LOG_ENGINE_INFO("[ACCEPT] buy request accepted line={} item={}", action.source_line, action.item_id);
        advanceAction();
    }

    inline void ClientAcceptanceScript::applyInput(Net::InputState& state, bool movement_allowed, bool can_fire, bool can_reload) const
    {
        if (!m_enabled || m_index >= m_actions.size())
            return;

        const Action& action = m_actions[m_index];
        using namespace Net::InputFlags;
        if (m_state == State::RunningMoveToLive) {
            if (!movement_allowed)
                return;
            // The acceptance path changes only the ordinary movement command
            // and its yaw. Completion observes the separately latched
            // reconciliation position, never this predicted Transform.
            state.buttons &= static_cast<uint16_t>(~(MOVE_FORWARD | MOVE_BACK | MOVE_LEFT | MOVE_RIGHT));
            state.buttons |= MOVE_FORWARD;
            state.move_forward = m_move_forward;
            state.move_right = 0.0f;
            state.camera_yaw = m_move_yaw;
            return;
        }
        if (m_state == State::RunningHoldUseLive || m_state == State::RunningHoldUseDefuse) {
            state.buttons |= USE;
            return;
        }
        if (m_state != State::RunningInput)
            return;

        const uint16_t scripted = action.input_buttons;
        const uint16_t movement_bits = scripted & (MOVE_FORWARD | MOVE_BACK | MOVE_LEFT | MOVE_RIGHT | JUMP | CROUCH | SPRINT | WALK);
        if (movement_allowed) {
            state.buttons |= movement_bits;
            if ((movement_bits & MOVE_FORWARD) != 0) state.move_forward += 1.0f;
            if ((movement_bits & MOVE_BACK) != 0) state.move_forward -= 1.0f;
            if ((movement_bits & MOVE_RIGHT) != 0) state.move_right += 1.0f;
            if ((movement_bits & MOVE_LEFT) != 0) state.move_right -= 1.0f;
        }
        state.buttons |= scripted & (USE | DROP);
        if (can_fire) state.buttons |= scripted & ATTACK;
        if (can_reload) state.buttons |= scripted & RELOAD;
        state.move_forward = std::clamp(state.move_forward, -1.0f, 1.0f);
        state.move_right = std::clamp(state.move_right, -1.0f, 1.0f);
    }

}
