#pragma once

#include "Network/NetworkInputState.hpp"
#include "shared/CompetitiveLogic.hpp"
#include "shared/WeaponTypes.hpp"

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
        WaitWorldItemPresent,
        WaitWorldItemRemoved,
        MoveToWorldItemLive,
        SelectSlot,
        TapDrop,
        WaitWeaponState,
        WaitWeaponStateFromItem,
        WaitBombPlanted,
        HoldUseLive,
        HoldUseDefuse,
        Wait
    };

    // These are observations of the template-owned client presentation index.
    // They are receipts from accepted server-to-client messages, not replicated
    // entities or client-side pickup authority.
    struct ClientAcceptanceWorldItem
    {
        uint32_t world_item_id = 0;
        float position_x = 0.0f;
        float position_z = 0.0f;
        uint64_t upsert_receipt_generation = 0;
    };

    struct ClientAcceptanceWorldItemRemoval
    {
        uint32_t world_item_id = 0;
        uint64_t receipt_generation = 0;
        // The owner-only WEAPON_STATE generation that had already been
        // accepted when this removal was applied.  A later owner receipt can
        // therefore be tied to this exact removal even when both messages are
        // drained before the next action-script tick.
        uint64_t owner_weapon_state_generation_at_receipt = 0;
    };

    // A durable receipt cache is separate from presentation rows so a pickup
    // action can compare the later owner weapon state after the item is gone.
    struct ClientAcceptanceWorldItemWeaponReceipt
    {
        uint32_t world_item_id = 0;
        WeaponType weapon_type = WeaponType::COUNT;
        uint64_t receipt_generation = 0;
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
        std::vector<ClientAcceptanceWorldItem> world_items;
        std::vector<ClientAcceptanceWorldItemRemoval> world_item_removals;
        std::vector<ClientAcceptanceWorldItemWeaponReceipt> world_item_weapon_receipts;
        uint64_t world_item_receipt_generation = 0;
        bool has_world_item_presentation_world = false;
        bool has_authoritative_weapon_state = false;
        WeaponType authoritative_weapon_type = WeaponType::COUNT;
        uint64_t authoritative_weapon_state_generation = 0;
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
        void noteInputCommandSent(uint32_t command_tick, bool steering_move_emitted,
                                  uint64_t weapon_state_generation = 0);
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
            uint32_t world_item_id = 0;
            uint16_t input_buttons = 0;
            uint8_t weapon_select = 0;
            WeaponType weapon_type = WeaponType::COUNT;
            float target_x = 0.0f;
            float target_z = 0.0f;
            float tolerance = 0.0f;
            uint32_t duration_ms = 0;
            uint32_t source_line = 0;
        };

        enum class State : uint8_t { Ready, WaitingForTeamResult, WaitingForBuyResult, RunningInput,
            RunningMoveToLive, WaitingForWorldItemPresent, WaitingForWorldItemRemoved,
            RunningMoveToWorldItemLive, RunningSelectSlot, RunningTapDrop,
            WaitingForWeaponState, WaitingForBombPlanted, RunningHoldUseLive,
            RunningHoldUseDefuse, Waiting };

        bool loadFile(const char* path, std::string& error);
        void disable(const std::string& reason);
        bool canDispatchTeam(const ClientAcceptanceObservation& observation) const;
        bool canDispatchPawnAction(const ClientAcceptanceObservation& observation) const;
        bool canRunLivePawnAction(const ClientAcceptanceObservation& observation) const;
        bool canRunWeaponLifecycleAction(const ClientAcceptanceObservation& observation) const;
        bool canWaitForBombPlanted(const ClientAcceptanceObservation& observation) const;
        const ClientAcceptanceWorldItem* findWorldItem(
            uint32_t world_item_id, const ClientAcceptanceObservation& observation) const;
        const ClientAcceptanceWorldItemRemoval* findWorldItemRemovalAfter(
            uint32_t world_item_id, uint64_t receipt_generation,
            const ClientAcceptanceObservation& observation) const;
        bool hasWorldItemRemovalAfter(uint32_t world_item_id, uint64_t receipt_generation,
                                      const ClientAcceptanceObservation& observation) const;
        const ClientAcceptanceWorldItemWeaponReceipt* findWorldItemWeaponReceipt(
            uint32_t world_item_id, const ClientAcceptanceObservation& observation) const;
        bool hasExpectedWeaponStateAfter(const ClientAcceptanceObservation& observation) const;
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
        uint64_t m_world_item_removal_start_generation = 0;
        // A target remove observed only after ordinary steering must survive
        // the transition to the following receipt action.  It is evidence of
        // disappearance, not evidence that this client won the pickup.
        uint32_t m_causal_world_item_removal_id = 0;
        uint64_t m_causal_world_item_removal_generation = 0;
        uint64_t m_causal_world_item_removal_weapon_state_generation = 0;
        uint32_t m_causal_world_item_removal_input_tick = 0;
        uint64_t m_weapon_state_wait_start_generation = 0;
        WeaponType m_expected_weapon_type = WeaponType::COUNT;
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

    inline bool parseWeaponSelect(const std::string& token, uint8_t& out)
    {
        if (token == "primary") { out = 1; return true; }
        if (token == "secondary") { out = 2; return true; }
        if (token == "melee") { out = 3; return true; }
        if (token == "grenade") { out = 4; return true; }
        if (token == "utility") { out = 5; return true; }
        return false;
    }

    inline bool parseWeaponAlias(const std::string& token, WeaponType& out)
    {
        if (token == "rifle") { out = WeaponType::RIFLE; return true; }
        if (token == "shotgun") { out = WeaponType::SHOTGUN; return true; }
        if (token == "pistol") { out = WeaponType::PISTOL; return true; }
        if (token == "knife") { out = WeaponType::KNIFE; return true; }
        if (token == "grenade") { out = WeaponType::GRENADE; return true; }
        const WeaponCatalogDef* definition = findCatalogByAlias(token.c_str());
        if (!definition || !hasWeaponDefinition(definition->weapon_type))
            return false;
        out = definition->weapon_type;
        return true;
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
        m_world_item_removal_start_generation = 0;
        m_causal_world_item_removal_id = 0;
        m_causal_world_item_removal_generation = 0;
        m_causal_world_item_removal_weapon_state_generation = 0;
        m_causal_world_item_removal_input_tick = 0;
        m_weapon_state_wait_start_generation = 0;
        m_expected_weapon_type = WeaponType::COUNT;
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
            } else if (tokens[0] == "wait_world_item_present" && tokens.size() == 3) {
                if (!parseUnsigned(tokens[1], action.world_item_id) || action.world_item_id == 0 ||
                    !parseUnsigned(tokens[2], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait_world_item_present on line " +
                        std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::WaitWorldItemPresent;
            } else if (tokens[0] == "wait_world_item_removed" && tokens.size() == 3) {
                if (!parseUnsigned(tokens[1], action.world_item_id) || action.world_item_id == 0 ||
                    !parseUnsigned(tokens[2], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait_world_item_removed on line " +
                        std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::WaitWorldItemRemoved;
            } else if (tokens[0] == "move_to_world_item_live" && tokens.size() == 4) {
                if (!parseUnsigned(tokens[1], action.world_item_id) || action.world_item_id == 0 ||
                    !parseFiniteFloat(tokens[2], action.tolerance) ||
                    !parseUnsigned(tokens[3], action.duration_ms) ||
                    action.tolerance <= 0.0f || action.tolerance > kMaxMoveTolerance ||
                    action.duration_ms == 0 || action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid move_to_world_item_live on line " +
                        std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::MoveToWorldItemLive;
            } else if (tokens[0] == "select_slot" && tokens.size() == 3) {
                if (!parseWeaponSelect(tokens[1], action.weapon_select) ||
                    !parseUnsigned(tokens[2], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid select_slot on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::SelectSlot;
            } else if (tokens[0] == "tap_drop" && tokens.size() == 2) {
                if (!parseUnsigned(tokens[1], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid tap_drop on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::TapDrop;
            } else if (tokens[0] == "wait_weapon_state" && tokens.size() == 3) {
                if (!parseWeaponAlias(tokens[1], action.weapon_type) ||
                    !parseUnsigned(tokens[2], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait_weapon_state on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::WaitWeaponState;
            } else if (tokens[0] == "wait_weapon_state_from_item" && tokens.size() == 3) {
                if (!parseUnsigned(tokens[1], action.world_item_id) || action.world_item_id == 0 ||
                    !parseUnsigned(tokens[2], action.duration_ms) || action.duration_ms == 0 ||
                    action.duration_ms > kMaxDurationMs) {
                    error = "client action script disabled: invalid wait_weapon_state_from_item on line " + std::to_string(line_number);
                    return false;
                }
                action.kind = ClientAcceptanceActionKind::WaitWeaponStateFromItem;
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

    inline bool ClientAcceptanceScript::canRunWeaponLifecycleAction(const ClientAcceptanceObservation& observation) const
    {
        using Phase = Competitive::RoundPhase;
        return canDispatchPawnAction(observation) &&
            (observation.round_phase == Phase::Warmup || observation.round_phase == Phase::Freeze ||
             observation.round_phase == Phase::Live);
    }

    inline bool ClientAcceptanceScript::canWaitForBombPlanted(const ClientAcceptanceObservation& observation) const
    {
        return canDispatchPawnAction(observation) &&
            observation.round_phase == Competitive::RoundPhase::Live;
    }

    inline const ClientAcceptanceWorldItem* ClientAcceptanceScript::findWorldItem(
        uint32_t world_item_id, const ClientAcceptanceObservation& observation) const
    {
        const auto item = std::find_if(observation.world_items.begin(), observation.world_items.end(),
            [world_item_id](const ClientAcceptanceWorldItem& candidate) {
                return candidate.world_item_id == world_item_id;
            });
        return item == observation.world_items.end() ? nullptr : &*item;
    }

    inline const ClientAcceptanceWorldItemRemoval* ClientAcceptanceScript::findWorldItemRemovalAfter(
        uint32_t world_item_id, uint64_t receipt_generation,
        const ClientAcceptanceObservation& observation) const
    {
        const auto removal = std::find_if(observation.world_item_removals.begin(), observation.world_item_removals.end(),
            [world_item_id, receipt_generation](const ClientAcceptanceWorldItemRemoval& removal) {
                return removal.world_item_id == world_item_id &&
                    removal.receipt_generation > receipt_generation;
            });
        return removal == observation.world_item_removals.end() ? nullptr : &*removal;
    }

    inline bool ClientAcceptanceScript::hasWorldItemRemovalAfter(
        uint32_t world_item_id, uint64_t receipt_generation,
        const ClientAcceptanceObservation& observation) const
    {
        return findWorldItemRemovalAfter(world_item_id, receipt_generation, observation) != nullptr;
    }

    inline const ClientAcceptanceWorldItemWeaponReceipt* ClientAcceptanceScript::findWorldItemWeaponReceipt(
        uint32_t world_item_id, const ClientAcceptanceObservation& observation) const
    {
        const auto receipt = std::find_if(observation.world_item_weapon_receipts.begin(),
            observation.world_item_weapon_receipts.end(), [world_item_id](const auto& candidate) {
                return candidate.world_item_id == world_item_id;
            });
        return receipt == observation.world_item_weapon_receipts.end() ? nullptr : &*receipt;
    }

    inline bool ClientAcceptanceScript::hasExpectedWeaponStateAfter(
        const ClientAcceptanceObservation& observation) const
    {
        return observation.has_authoritative_weapon_state &&
            observation.authoritative_weapon_state_generation > m_weapon_state_wait_start_generation &&
            (m_expected_weapon_type == WeaponType::COUNT ||
             observation.authoritative_weapon_type == m_expected_weapon_type);
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
        if (m_causal_world_item_removal_id != 0) {
            // A pickup receipt may be observed while ordinary movement is
            // still the active action. Keep that exact remove boundary across
            // the documented acknowledgement path:
            // move -> wait removed -> select slot -> wait item weapon state.
            // The final wait still requires a newer matching owner-only
            // WEAPON_STATE, so neither the remove nor an old owner state can
            // complete the pickup action on its own.
            size_t candidate_index = m_index;
            if (candidate_index < m_actions.size() &&
                m_actions[candidate_index].kind == ClientAcceptanceActionKind::WaitWorldItemRemoved &&
                m_actions[candidate_index].world_item_id == m_causal_world_item_removal_id) {
                ++candidate_index;
            }
            if (candidate_index < m_actions.size() &&
                m_actions[candidate_index].kind == ClientAcceptanceActionKind::SelectSlot) {
                ++candidate_index;
            }
            const bool next_action_can_consume_causal_removal = candidate_index < m_actions.size() &&
                m_actions[candidate_index].kind == ClientAcceptanceActionKind::WaitWeaponStateFromItem &&
                m_actions[candidate_index].world_item_id == m_causal_world_item_removal_id;
            if (!next_action_can_consume_causal_removal) {
                m_causal_world_item_removal_id = 0;
                m_causal_world_item_removal_generation = 0;
                m_causal_world_item_removal_weapon_state_generation = 0;
                m_causal_world_item_removal_input_tick = 0;
            }
        }
        m_state = State::Ready;
        m_elapsed_seconds = 0.0f;
        m_move_yaw = 0.0f;
        m_move_forward = 0.0f;
        m_move_start_snapshot_generation = 0;
        m_world_item_removal_start_generation = 0;
        m_weapon_state_wait_start_generation = 0;
        m_expected_weapon_type = WeaponType::COUNT;
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
            } else if (action.kind == ClientAcceptanceActionKind::WaitWorldItemPresent) {
                if (!observation.connected || !observation.has_world_item_presentation_world) return dispatch;
                m_state = State::WaitingForWorldItemPresent;
                m_elapsed_seconds = 0.0f;
                LOG_ENGINE_INFO("[ACCEPT] wait_world_item_present begin line={} id={} timeout_ms={}",
                    action.source_line, action.world_item_id, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::WaitWorldItemRemoved) {
                if (m_causal_world_item_removal_id == action.world_item_id) {
                    LOG_ENGINE_INFO(
                        "[ACCEPT] wait_world_item_removed acknowledged movement-causal receipt line={} id={} receipt={} input_tick={}",
                        action.source_line, action.world_item_id, m_causal_world_item_removal_generation,
                        m_causal_world_item_removal_input_tick);
                    // Keep the receipt boundary until the documented later
                    // wait_weapon_state_from_item claims it. This lets an
                    // intervening ordinary select-slot acknowledgement arrive
                    // before the item-state wait begins without erasing the
                    // causal pickup boundary.
                    advanceAction();
                    return dispatch;
                }
                if (!observation.connected || !observation.has_world_item_presentation_world ||
                    !findWorldItem(action.world_item_id, observation)) return dispatch;
                m_state = State::WaitingForWorldItemRemoved;
                m_elapsed_seconds = 0.0f;
                m_world_item_removal_start_generation = observation.world_item_receipt_generation;
                LOG_ENGINE_INFO("[ACCEPT] wait_world_item_removed begin line={} id={} timeout_ms={}",
                    action.source_line, action.world_item_id, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::MoveToWorldItemLive) {
                const ClientAcceptanceWorldItem* item = findWorldItem(action.world_item_id, observation);
                if (!item || !observation.has_world_item_presentation_world || !canRunLivePawnAction(observation)) return dispatch;
                action.target_x = item->position_x;
                action.target_z = item->position_z;
                m_state = State::RunningMoveToWorldItemLive;
                m_elapsed_seconds = 0.0f;
                m_move_start_snapshot_generation = observation.authoritative_pawn_snapshot_generation;
                m_world_item_removal_start_generation = observation.world_item_receipt_generation;
                m_causal_world_item_removal_id = 0;
                m_causal_world_item_removal_generation = 0;
                m_causal_world_item_removal_weapon_state_generation = 0;
                m_causal_world_item_removal_input_tick = 0;
                m_move_steering_input_tick = 0;
                updateMoveYaw(action, observation);
                LOG_ENGINE_INFO("[ACCEPT] move_to_world_item_live begin line={} id={} x={} z={} tolerance={} timeout_ms={}",
                    action.source_line, action.world_item_id, action.target_x, action.target_z,
                    action.tolerance, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::SelectSlot) {
                if (!canRunWeaponLifecycleAction(observation)) return dispatch;
                m_state = State::RunningSelectSlot;
                m_elapsed_seconds = 0.0f;
                m_expected_weapon_type = WeaponType::COUNT;
                LOG_ENGINE_INFO("[ACCEPT] select_slot begin line={} select={} timeout_ms={}",
                    action.source_line, static_cast<unsigned>(action.weapon_select), action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::TapDrop) {
                if (!canRunWeaponLifecycleAction(observation)) return dispatch;
                m_state = State::RunningTapDrop;
                m_elapsed_seconds = 0.0f;
                m_expected_weapon_type = WeaponType::COUNT;
                LOG_ENGINE_INFO("[ACCEPT] tap_drop begin line={} timeout_ms={}",
                    action.source_line, action.duration_ms);
            } else if (action.kind == ClientAcceptanceActionKind::WaitWeaponState ||
                       action.kind == ClientAcceptanceActionKind::WaitWeaponStateFromItem) {
                if (!canRunWeaponLifecycleAction(observation)) return dispatch;
                if (action.kind == ClientAcceptanceActionKind::WaitWeaponStateFromItem) {
                    const ClientAcceptanceWorldItemWeaponReceipt* item_receipt =
                        findWorldItemWeaponReceipt(action.world_item_id, observation);
                    if (!item_receipt || !hasWeaponDefinition(item_receipt->weapon_type)) {
                        disable("client action script stopped: wait_weapon_state_from_item lacks accepted item receipt on line " +
                            std::to_string(action.source_line));
                        return dispatch;
                    }
                    m_expected_weapon_type = item_receipt->weapon_type;
                } else {
                    m_expected_weapon_type = action.weapon_type;
                }
                m_state = State::WaitingForWeaponState;
                m_elapsed_seconds = 0.0f;
                // If a move action reached an accepted target removal in the
                // preceding tick, retain the generation captured by the
                // removal callback. This accepts only an owner state received
                // after that exact removal, even if the transport drained both
                // messages before this action first runs. Removal alone never
                // completes this weapon wait.
                if (action.kind == ClientAcceptanceActionKind::WaitWeaponStateFromItem &&
                    m_causal_world_item_removal_id == action.world_item_id) {
                    m_weapon_state_wait_start_generation =
                        m_causal_world_item_removal_weapon_state_generation;
                    m_causal_world_item_removal_id = 0;
                    m_causal_world_item_removal_generation = 0;
                    m_causal_world_item_removal_weapon_state_generation = 0;
                    m_causal_world_item_removal_input_tick = 0;
                } else {
                    m_weapon_state_wait_start_generation = observation.authoritative_weapon_state_generation;
                }
                LOG_ENGINE_INFO("[ACCEPT] wait_weapon_state begin line={} expected_weapon={} timeout_ms={}",
                    action.source_line, static_cast<unsigned>(m_expected_weapon_type), action.duration_ms);
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
        case State::RunningSelectSlot:
        case State::RunningTapDrop:
            if (!canRunWeaponLifecycleAction(observation)) {
                disable("client action script stopped: normal input action lost pawn or lifecycle authority on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: normal input action was not sent on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::WaitingForWeaponState:
            if (!canRunWeaponLifecycleAction(observation)) {
                disable("client action script stopped: wait_weapon_state lost pawn or lifecycle authority on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (hasExpectedWeaponStateAfter(observation)) {
                LOG_ENGINE_INFO("[ACCEPT] wait_weapon_state observed owner receipt line={} weapon={} generation={}",
                    action.source_line, static_cast<unsigned>(observation.authoritative_weapon_type),
                    observation.authoritative_weapon_state_generation);
                advanceAction();
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: wait_weapon_state timed out on line " +
                    std::to_string(action.source_line));
            }
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
        case State::WaitingForWorldItemPresent:
            if (!observation.connected || !observation.has_world_item_presentation_world) {
                disable("client action script stopped: wait_world_item_present lost connection or client world on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (findWorldItem(action.world_item_id, observation)) {
                LOG_ENGINE_INFO("[ACCEPT] wait_world_item_present observed accepted receipt line={} id={}",
                    action.source_line, action.world_item_id);
                advanceAction();
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: wait_world_item_present timed out on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::WaitingForWorldItemRemoved:
            if (!observation.connected || !observation.has_world_item_presentation_world) {
                disable("client action script stopped: wait_world_item_removed lost connection or client world on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (hasWorldItemRemovalAfter(action.world_item_id, m_world_item_removal_start_generation, observation)) {
                LOG_ENGINE_INFO("[ACCEPT] wait_world_item_removed observed accepted receipt line={} id={}",
                    action.source_line, action.world_item_id);
                advanceAction();
                return dispatch;
            }
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: wait_world_item_removed timed out on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        case State::RunningMoveToWorldItemLive: {
            const ClientAcceptanceWorldItem* item = findWorldItem(action.world_item_id, observation);
            if (!item || !observation.has_world_item_presentation_world) {
                const ClientAcceptanceWorldItemRemoval* removal = findWorldItemRemovalAfter(
                    action.world_item_id, m_world_item_removal_start_generation, observation);
                if (removal && m_move_steering_input_tick != 0) {
                    // A matching, newer removal after an actual ordinary
                    // steering command can race the final reconciliation
                    // snapshot. Carry only the receipt identity, its ordering
                    // boundary, and the command tick to the next action. Do
                    // not treat this as a pickup: the following owner-only
                    // WEAPON_STATE still has to match the cached item type.
                    m_causal_world_item_removal_id = removal->world_item_id;
                    m_causal_world_item_removal_generation = removal->receipt_generation;
                    m_causal_world_item_removal_weapon_state_generation =
                        removal->owner_weapon_state_generation_at_receipt;
                    m_causal_world_item_removal_input_tick = m_move_steering_input_tick;
                    LOG_ENGINE_INFO(
                        "[ACCEPT] move_to_world_item_live observed movement-causal target removal line={} id={} receipt={} input_tick={} owner_weapon_generation={}",
                        action.source_line, action.world_item_id, removal->receipt_generation,
                        m_move_steering_input_tick, removal->owner_weapon_state_generation_at_receipt);
                    advanceAction();
                    return dispatch;
                }
                disable("client action script stopped: move_to_world_item_live item disappeared on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            if (!canRunLivePawnAction(observation)) {
                disable("client action script stopped: move_to_world_item_live lost authoritative Live pawn state on line " +
                    std::to_string(action.source_line));
                return dispatch;
            }
            action.target_x = item->position_x;
            action.target_z = item->position_z;
            if (moveHasAuthoritativeArrival(action, observation)) {
                LOG_ENGINE_INFO("[ACCEPT] move_to_world_item_live arrived line={} id={} authoritative_snapshot={} acknowledged_input={}",
                    action.source_line, action.world_item_id, observation.authoritative_pawn_snapshot_generation,
                    observation.last_authoritative_input_tick);
                advanceAction();
                return dispatch;
            }
            updateMoveYaw(action, observation);
            m_elapsed_seconds += std::max(delta_time, 0.0f);
            if (m_elapsed_seconds * 1000.0f >= static_cast<float>(action.duration_ms)) {
                disable("client action script stopped: move_to_world_item_live timed out on line " +
                    std::to_string(action.source_line));
            }
            return dispatch;
        }
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
        return m_state == State::RunningMoveToLive ||
            m_state == State::RunningMoveToWorldItemLive;
    }

    inline void ClientAcceptanceScript::noteInputCommandSent(uint32_t command_tick,
                                                               bool steering_move_emitted,
                                                               uint64_t weapon_state_generation)
    {
        if (!m_enabled || m_index >= m_actions.size())
            return;
        if (isMoveToLiveSteering() && steering_move_emitted &&
            m_move_steering_input_tick == 0 && command_tick != 0) {
            m_move_steering_input_tick = command_tick;
            return;
        }
        if (m_state == State::RunningSelectSlot || m_state == State::RunningTapDrop) {
            const Action& action = m_actions[m_index];
            m_state = State::WaitingForWeaponState;
            m_elapsed_seconds = 0.0f;
            // This baseline is sampled exactly after the one ordinary input
            // command is enqueued, so an older owner state cannot satisfy the
            // action even if it arrives later in the frame.
            m_weapon_state_wait_start_generation = weapon_state_generation;
            LOG_ENGINE_INFO("[ACCEPT] {} command sent line={} tick={} weapon_generation={}",
                action.kind == ClientAcceptanceActionKind::SelectSlot ? "select_slot" : "tap_drop",
                action.source_line, command_tick, weapon_state_generation);
        }
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
        if (isMoveToLiveSteering()) {
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
        if (m_state == State::RunningSelectSlot) {
            state.buttons = Net::withWeaponSelect(state.buttons, action.weapon_select);
            return;
        }
        if (m_state == State::RunningTapDrop) {
            state.buttons &= static_cast<uint16_t>(~DROP);
            state.buttons |= DROP;
            return;
        }
        if (m_state == State::WaitingForWeaponState &&
            (action.kind == ClientAcceptanceActionKind::SelectSlot ||
             action.kind == ClientAcceptanceActionKind::TapDrop)) {
            // The driver guarantees that its edge is present in exactly one
            // ordinary command. It neither holds the bit nor emits a custom
            // acceptance message while waiting for the server receipt.
            state.buttons = Net::withWeaponSelect(state.buttons, 0);
            state.buttons &= static_cast<uint16_t>(~DROP);
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
