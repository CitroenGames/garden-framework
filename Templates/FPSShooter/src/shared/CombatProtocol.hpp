#pragma once

#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "WeaponTypes.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

enum class CombatMessageType : uint8_t
{
    SHOOT_RESULT = Net::CUSTOM_MESSAGE_START,
    DAMAGE_EVENT = Net::CUSTOM_MESSAGE_START + 1,
    PLAYER_DIED = Net::CUSTOM_MESSAGE_START + 2,
    PLAYER_RESPAWN = Net::CUSTOM_MESSAGE_START + 3,
    WEAPON_STATE = Net::CUSTOM_MESSAGE_START + 4
};

// Competitive mode custom-message range (starting at CUSTOM_MESSAGE_START+5 = 69).
enum class CompetitiveMessageType : uint8_t
{
    ROUND_STATE = Net::CUSTOM_MESSAGE_START + 5,    // 69 S->C reliable
    ECONOMY_STATE = Net::CUSTOM_MESSAGE_START + 6,  // 70 S->C owner-only reliable
    BUY_REQUEST = Net::CUSTOM_MESSAGE_START + 7,    // 71 C->S reliable
    BUY_RESULT = Net::CUSTOM_MESSAGE_START + 8,     // 72 S->C reliable
    TEAM_SELECT = Net::CUSTOM_MESSAGE_START + 9,    // 73 C->S reliable
    TEAM_RESULT = Net::CUSTOM_MESSAGE_START + 10    // 74 S->C reliable
};

// Template-owned durable world-item presentation messages. These values are
// reserved after the competitive range and must remain distinct from generic
// EngineCore message types and future template custom messages.
enum class WorldItemMessageType : uint8_t
{
    WORLD_ITEM_UPSERT = Net::CUSTOM_MESSAGE_START + 11, // 75 S->C reliable
    WORLD_ITEM_REMOVE = Net::CUSTOM_MESSAGE_START + 12  // 76 S->C reliable
};

// Versioned all-client combat publication.  This remains a template payload:
// EngineCore only gates the exact protocol version that admits it.
enum class CombatEventMessageType : uint8_t
{
    COMBAT_EVENT = Net::CUSTOM_MESSAGE_START + 13 // 77 S->C reliable
};

// Recipient-specific durable competitive presentation. This is deliberately a
// template protocol: EngineCore only exact-gates the version that admits it.
enum class CompetitivePresentationMessageType : uint8_t
{
    SNAPSHOT = Net::CUSTOM_MESSAGE_START + 14 // 78 S->C reliable
};

constexpr uint8_t kCompetitivePresentationSchemaVersion = 2;
constexpr uint8_t kCompetitivePresentationMaxRows = 10; // current 5v5 capacity
constexpr uint8_t kCompetitivePresentationMaxRadarRecords = 11;
constexpr size_t kCompetitivePresentationMaxNameChars = Net::NETWORK_MAX_PLAYER_NAME_CHARS;
constexpr size_t kCompetitivePresentationMaxPayloadBytes = 1024;

enum class CompetitivePresentationRosterState : uint8_t
{
    Awaiting = 0,
    Alive = 1,
    Dead = 2,
    Spectating = 3
};

enum class CompetitivePresentationRadarKind : uint8_t
{
    Self = 0,
    Teammate = 1,
    Objective = 2
};

struct CompetitivePresentationRosterRow
{
    uint32_t stable_player_id = 0;
    std::string display_name;
    uint8_t team = 0;
    uint8_t state = static_cast<uint8_t>(CompetitivePresentationRosterState::Awaiting);
    uint16_t kills = 0;
    uint16_t deaths = 0;
    uint8_t is_bot = 0;
    uint8_t money_visible = 0;
    int32_t money = 0;
    uint16_t ping_ms = 0;
};

struct CompetitivePresentationRadarRecord
{
    uint32_t stable_record_id = 0;
    uint8_t kind = static_cast<uint8_t>(CompetitivePresentationRadarKind::Self);
    glm::vec3 position = glm::vec3(0.0f);
};

struct CompetitivePresentationSnapshotMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitivePresentationMessageType::SNAPSHOT);
    uint8_t schema_version = kCompetitivePresentationSchemaVersion;
    uint32_t match_instance_id = 0;
    uint32_t presentation_revision = 0;
    uint32_t source_server_tick = 0;
    uint8_t recipient_team = 0;
    // The receiving player's match-scoped presentation identity is durable
    // recipient context. It identifies the local roster row even when the
    // receiver is dead and therefore has no Self radar record.
    uint32_t recipient_stable_player_id = 0;
    uint8_t phase = 0;
    uint32_t phase_deadline_tick = 0;
    uint16_t round_number = 0;
    uint8_t wins_attackers = 0;
    uint8_t wins_defenders = 0;
    uint8_t last_reason = 0;
    uint8_t last_winner = 0;
    uint8_t objective_state = 0;
    uint8_t objective_site = 0xff; // no site
    uint32_t objective_deadline_tick = 0;
    uint8_t channel_kind = 0; // none, plant, defuse
    uint32_t channel_actor_stable_id = 0;
    uint32_t channel_deadline_tick = 0;
    uint8_t roster_count = 0;
    std::array<CompetitivePresentationRosterRow, kCompetitivePresentationMaxRows> roster{};
    uint8_t radar_count = 0;
    std::array<CompetitivePresentationRadarRecord, kCompetitivePresentationMaxRadarRecords> radar{};
};

constexpr bool isValidCompetitivePresentationTeam(uint8_t team) { return team <= 3; }
constexpr bool isValidCompetitivePresentationPhase(uint8_t phase) { return phase <= 6; }
constexpr bool isValidCompetitivePresentationReason(uint8_t reason) { return reason <= 7; }
constexpr bool isValidCompetitivePresentationWinner(uint8_t winner) { return winner <= 3; }
constexpr bool isValidCompetitivePresentationObjectiveState(uint8_t state) { return state <= 7; }
constexpr bool isValidCompetitivePresentationObjectiveSite(uint8_t site) { return site == 0xff || site <= 7; }
constexpr bool isValidCompetitivePresentationChannelKind(uint8_t kind) { return kind <= 2; }
constexpr bool isValidCompetitivePresentationRosterState(uint8_t state) { return state <= static_cast<uint8_t>(CompetitivePresentationRosterState::Spectating); }
constexpr bool isValidCompetitivePresentationRadarKind(uint8_t kind) { return kind <= static_cast<uint8_t>(CompetitivePresentationRadarKind::Objective); }

// Conservative v1 recipient policy: a playing recipient sees balances and
// positions only for its own playing side. Spectators get no wallet or radar
// records until a separate observer policy defines that information.
constexpr bool competitivePresentationAllowsTeamPrivateData(
    uint8_t recipient_team, bool recipient_is_spectator, uint8_t subject_team)
{
    return !recipient_is_spectator && (recipient_team == 1 || recipient_team == 2) &&
        subject_team == recipient_team;
}

// Serial-number ordering for one match instance.  Callers must reject a
// different instance before asking this helper about the revision.
constexpr bool isCompetitivePresentationRevisionNewer(uint32_t candidate, uint32_t accepted)
{
    return candidate != accepted && static_cast<uint32_t>(candidate - accepted) < 0x80000000u;
}

inline bool isValidCompetitivePresentationName(std::string_view name)
{
    return !name.empty() && name.size() <= kCompetitivePresentationMaxNameChars &&
        Net::normalizePlayerName(name) == name;
}

inline bool isFiniteCompetitivePresentationPosition(const glm::vec3& position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

inline bool isValidCompetitivePresentationSnapshot(const CompetitivePresentationSnapshotMessage& message)
{
    if (message.type != static_cast<uint8_t>(CompetitivePresentationMessageType::SNAPSHOT) ||
        message.schema_version != kCompetitivePresentationSchemaVersion ||
        message.match_instance_id == 0 || message.presentation_revision == 0 ||
        !isValidCompetitivePresentationTeam(message.recipient_team) ||
        message.recipient_stable_player_id == 0 ||
        !isValidCompetitivePresentationPhase(message.phase) ||
        !isValidCompetitivePresentationReason(message.last_reason) ||
        !isValidCompetitivePresentationWinner(message.last_winner) ||
        !isValidCompetitivePresentationObjectiveState(message.objective_state) ||
        !isValidCompetitivePresentationObjectiveSite(message.objective_site) ||
        !isValidCompetitivePresentationChannelKind(message.channel_kind) ||
        message.roster_count > kCompetitivePresentationMaxRows ||
        message.radar_count > kCompetitivePresentationMaxRadarRecords)
        return false;
    if (message.channel_kind == 0 &&
        (message.channel_actor_stable_id != 0 || message.channel_deadline_tick != 0))
        return false;
    if (message.channel_kind != 0 &&
        (message.channel_actor_stable_id == 0 || message.channel_deadline_tick == 0))
        return false;

    uint32_t previous_row_id = 0;
    for (uint8_t index = 0; index < message.roster_count; ++index) {
        const auto& row = message.roster[index];
        if (row.stable_player_id == 0 || row.stable_player_id <= previous_row_id ||
            !isValidCompetitivePresentationName(row.display_name) ||
            !isValidCompetitivePresentationTeam(row.team) ||
            !isValidCompetitivePresentationRosterState(row.state) ||
            row.is_bot > 1 || row.money_visible > 1 || (!row.money_visible && row.money != 0))
            return false;
        previous_row_id = row.stable_player_id;
    }

    const auto findRosterRow = [&message](uint32_t stable_player_id)
        -> const CompetitivePresentationRosterRow* {
        for (uint8_t row_index = 0; row_index < message.roster_count; ++row_index) {
            if (message.roster[row_index].stable_player_id == stable_player_id)
                return &message.roster[row_index];
        }
        return nullptr;
    };

    uint32_t previous_radar_id = 0;
    for (uint8_t index = 0; index < message.radar_count; ++index) {
        const auto& record = message.radar[index];
        if (record.stable_record_id == 0 || record.stable_record_id <= previous_radar_id ||
            !isValidCompetitivePresentationRadarKind(record.kind) ||
            !isFiniteCompetitivePresentationPosition(record.position))
            return false;
        const bool is_objective = record.kind == static_cast<uint8_t>(CompetitivePresentationRadarKind::Objective);
        const CompetitivePresentationRosterRow* row = findRosterRow(record.stable_record_id);
        const bool references_roster = row != nullptr;
        if ((is_objective && record.stable_record_id != std::numeric_limits<uint32_t>::max()) ||
            (!is_objective && !references_roster))
            return false;
        if (!is_objective) {
            const bool recipient_is_playing = message.recipient_team == 1 || message.recipient_team == 2;
            const bool row_is_alive = row->state == static_cast<uint8_t>(CompetitivePresentationRosterState::Alive);
            if (!recipient_is_playing || !row_is_alive || row->team != message.recipient_team)
                return false;
            if (record.kind == static_cast<uint8_t>(CompetitivePresentationRadarKind::Self) &&
                record.stable_record_id != message.recipient_stable_player_id)
                return false;
            if (record.kind == static_cast<uint8_t>(CompetitivePresentationRadarKind::Teammate) &&
                record.stable_record_id == message.recipient_stable_player_id)
                return false;
        }
        previous_radar_id = record.stable_record_id;
    }
    return true;
}

enum class WorldItemMessageAudience : uint8_t
{
    Invalid,
    ServerToClient
};

// Delivery provenance is explicit on every durable upsert so diagnostics and
// acceptance drivers never infer replay from unrelated session messages.
enum class WorldItemUpsertDelivery : uint8_t
{
    Live = 0,
    AdmissionReplay = 1
};

constexpr bool isValidWorldItemUpsertDelivery(uint8_t delivery)
{
    return delivery == static_cast<uint8_t>(WorldItemUpsertDelivery::Live) ||
        delivery == static_cast<uint8_t>(WorldItemUpsertDelivery::AdmissionReplay);
}

constexpr const char* worldItemUpsertDeliveryName(WorldItemUpsertDelivery delivery)
{
    switch (delivery) {
    case WorldItemUpsertDelivery::Live: return "live";
    case WorldItemUpsertDelivery::AdmissionReplay: return "replay";
    }
    return "invalid";
}

constexpr WorldItemMessageAudience worldItemMessageAudience(WorldItemMessageType type)
{
    switch (type) {
    case WorldItemMessageType::WORLD_ITEM_UPSERT:
    case WorldItemMessageType::WORLD_ITEM_REMOVE:
        return WorldItemMessageAudience::ServerToClient;
    }
    return WorldItemMessageAudience::Invalid;
}

constexpr bool isWorldItemServerToClientMessage(uint8_t type)
{
    return worldItemMessageAudience(static_cast<WorldItemMessageType>(type)) ==
        WorldItemMessageAudience::ServerToClient;
}

// Delivery scope is part of the game protocol contract.  ROUND_STATE is the
// shared match snapshot; wallet, team assignment, and buy outcome belong only
// to the client that owns them.  There is intentionally no generic inventory
// replication in this protocol: the existing owner-targeted WEAPON_STATE is
// the available active-weapon snapshot.
enum class CompetitiveMessageAudience : uint8_t
{
    Invalid,
    AllConnectedClients,
    OwningClient,
    Server
};

constexpr CompetitiveMessageAudience competitiveMessageAudience(CompetitiveMessageType type)
{
    switch (type) {
    case CompetitiveMessageType::ROUND_STATE: return CompetitiveMessageAudience::AllConnectedClients;
    case CompetitiveMessageType::ECONOMY_STATE:
    case CompetitiveMessageType::BUY_RESULT:
    case CompetitiveMessageType::TEAM_RESULT: return CompetitiveMessageAudience::OwningClient;
    case CompetitiveMessageType::BUY_REQUEST:
    case CompetitiveMessageType::TEAM_SELECT: return CompetitiveMessageAudience::Server;
    }
    return CompetitiveMessageAudience::Invalid;
}

constexpr bool isCompetitiveOwnerOnlyMessage(uint8_t type)
{
    return competitiveMessageAudience(static_cast<CompetitiveMessageType>(type)) ==
        CompetitiveMessageAudience::OwningClient;
}

struct RoundStateMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE);
    uint8_t phase = 0;
    float phase_time_remaining = 0.0f;
    uint32_t phase_deadline_tick = 0; // server tick at which phase ends
    uint32_t server_tick = 0;
    uint16_t round_number = 0;
    uint8_t wins_attackers = 0;
    uint8_t wins_defenders = 0;
    uint8_t bomb_planted = 0;
    uint8_t last_reason = 0;
    uint8_t last_winner = 0;
};

struct EconomyStateMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::ECONOMY_STATE);
    int32_t balance = 0;
    uint8_t loss_streak = 0;
};

struct BuyRequestMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::BUY_REQUEST);
    uint32_t request_seq = 0; // per-client, monotonically increasing (dedup)
    uint16_t item_id = 0;     // WeaponCatalogDef id
};

struct BuyResultMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::BUY_RESULT);
    uint32_t request_seq = 0;
    uint16_t item_id = 0;
    uint8_t result = 0; // Competitive::BuyResult
    int32_t balance = 0;
};

struct TeamSelectMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::TEAM_SELECT);
    uint8_t desired_team = 0; // Competitive::FpsTeam
    uint8_t auto_assign = 0;
};

struct TeamResultMessage
{
    uint8_t type = static_cast<uint8_t>(CompetitiveMessageType::TEAM_RESULT);
    uint8_t result = 0; // Competitive::TeamSelectResult
    uint8_t team = 0;   // Competitive::FpsTeam
};

// The yaw is radians on the wire, matching the existing network convention.
// Callers canonicalize it to [-pi, pi) before send; receivers validate that
// range so equivalent orientations cannot produce distinct replicated poses.
struct WorldItemUpsertMessage
{
    uint8_t type = static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_UPSERT);
    uint32_t world_item_id = 0;
    uint8_t delivery = static_cast<uint8_t>(WorldItemUpsertDelivery::Live);
    uint8_t weapon_type = 0;
    glm::vec3 position = glm::vec3(0.0f);
    float yaw = 0.0f;
};

struct WorldItemRemoveMessage
{
    uint8_t type = static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_REMOVE);
    uint32_t world_item_id = 0;
};

constexpr float kWorldItemYawPi = 3.14159265358979323846f;
constexpr float kWorldItemYawTwoPi = 2.0f * kWorldItemYawPi;

inline float normalizeWorldItemYaw(float yaw)
{
    if (!std::isfinite(yaw))
        return yaw;
    float normalized = std::fmod(yaw + kWorldItemYawPi, kWorldItemYawTwoPi);
    if (normalized < 0.0f)
        normalized += kWorldItemYawTwoPi;
    return normalized - kWorldItemYawPi;
}

inline bool isNormalizedWorldItemYaw(float yaw)
{
    return std::isfinite(yaw) && yaw >= -kWorldItemYawPi && yaw < kWorldItemYawPi;
}

inline bool isValidWorldItemPosition(const glm::vec3& position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        position.y >= kWorldItemMinimumWorldY;
}

inline bool isValidWorldItemWeaponType(uint8_t wire_weapon_type)
{
    const WeaponType weapon_type = static_cast<WeaponType>(wire_weapon_type);
    return hasWeaponDefinition(weapon_type) && weapon_type != WeaponType::KNIFE;
}

inline bool isValidWorldItemUpsert(const WorldItemUpsertMessage& message)
{
    return message.type == static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_UPSERT) &&
        message.world_item_id != 0 && isValidWorldItemUpsertDelivery(message.delivery) &&
        isValidWorldItemWeaponType(message.weapon_type) &&
        isValidWorldItemPosition(message.position) && isNormalizedWorldItemYaw(message.yaw);
}

inline bool isValidWorldItemRemove(const WorldItemRemoveMessage& message)
{
    return message.type == static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_REMOVE) &&
        message.world_item_id != 0;
}

// Client-to-server commands are validated structurally by the serializer and
// semantically before the server session or ECS is touched. Keep these wire
// bounds here so the server callback and focused protocol tests share one
// definition without moving FPS session policy into EngineCore.
constexpr bool isValidCompetitiveBuyRequest(const BuyRequestMessage& message)
{
    return message.request_seq != 0;
}

constexpr bool isValidCompetitiveTeamSelectRequest(const TeamSelectMessage& message)
{
    constexpr uint8_t kSpectatorTeamId = 3;
    return message.auto_assign <= 1 && message.desired_team <= kSpectatorTeamId;
}

struct ShootResultMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::SHOOT_RESULT);
    uint16_t shooter_client_id = 0;
    glm::vec3 ray_origin = glm::vec3(0);
    glm::vec3 hit_position = glm::vec3(0);
    uint32_t hit_entity_id = 0;
    uint8_t weapon_type = 0;
};

struct DamageEventMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::DAMAGE_EVENT);
    uint16_t attacker_client_id = 0;
    uint16_t victim_client_id = 0;
    int32_t damage = 0;
    int32_t health_remaining = 0;
    glm::vec3 hit_position = glm::vec3(0);
};

struct PlayerDiedMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::PLAYER_DIED);
    uint16_t victim_client_id = 0;
    uint16_t killer_client_id = 0;
    glm::vec3 death_position = glm::vec3(0);
};

// A death is identified by the victim's per-life sequence. publication_sequence
// carries the server's total feed order when multiple victims die in one tick.
// Score fields are the exact already-applied GameRules deltas; clients display
// them and must never recalculate authoritative score from attribution.
struct CombatEventMessage
{
    uint8_t type = static_cast<uint8_t>(CombatEventMessageType::COMBAT_EVENT);
    uint16_t killer_client_id = 0;
    uint16_t victim_client_id = 0;
    uint8_t weapon_type = 0;
    uint8_t flags = 0;
    uint32_t death_sequence = 0;
    uint64_t publication_sequence = 0;
    int8_t killer_kills = 0;
    int8_t victim_deaths = 0;
    uint8_t match_kill_recorded = 0;
};

constexpr uint8_t kCombatEventKnownFlags = 1u << 0; // self-inflicted

inline bool isValidCombatEventWeaponType(uint8_t wire_weapon_type)
{
    // COUNT is the existing template's explicit environmental/no-weapon
    // placeholder for authoritative non-weapon deaths.
    return wire_weapon_type <= static_cast<uint8_t>(WeaponType::COUNT);
}

inline bool isValidCombatEvent(const CombatEventMessage& message)
{
    if (message.type != static_cast<uint8_t>(CombatEventMessageType::COMBAT_EVENT) ||
        message.victim_client_id == 0 || message.death_sequence == 0 ||
        message.publication_sequence == 0 ||
        !isValidCombatEventWeaponType(message.weapon_type) ||
        (message.flags & ~kCombatEventKnownFlags) != 0 ||
        message.match_kill_recorded > 1)
        return false;

    const bool self_inflicted = (message.flags & kCombatEventKnownFlags) != 0;
    return self_inflicted == (message.killer_client_id == message.victim_client_id);
}

struct PlayerRespawnMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::PLAYER_RESPAWN);
    uint16_t client_id = 0;
    uint32_t entity_id = 0;
    glm::vec3 spawn_position = glm::vec3(0);
    int32_t health = 100;
};

struct WeaponStateMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::WEAPON_STATE);
    int32_t ammo = 0;
    int32_t max_ammo = 0;
    int32_t reserve_ammo = 0;
    int32_t max_reserve_ammo = 0;
    uint8_t weapon_type = 0;
    uint8_t reloading = 0;
    float fire_cooldown = 0.0f;
    float reload_timer = 0.0f;
    float spread = 0.0f;
    float accuracy_penalty = 0.0f;
    float recoil_index = 0.0f;
};

namespace CombatSerializer
{
    inline bool serialize(Net::BitWriter& writer, const WorldItemUpsertMessage& msg) {
        if (!isValidWorldItemUpsert(msg)) return false;
        writer.writeByte(msg.type);
        writer.writeUInt32(msg.world_item_id);
        writer.writeByte(msg.delivery);
        writer.writeByte(msg.weapon_type);
        writer.writeVector3f(msg.position);
        writer.writeFloat(msg.yaw);
        return true;
    }

    inline bool deserialize(Net::BitReader& reader, WorldItemUpsertMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_UPSERT)) return false;
        msg.world_item_id = reader.readUInt32();
        msg.delivery = reader.readByte();
        msg.weapon_type = reader.readByte();
        msg.position = reader.readVector3f();
        msg.yaw = reader.readFloat();
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8 &&
            isValidWorldItemUpsert(msg);
    }

    inline bool serialize(Net::BitWriter& writer, const WorldItemRemoveMessage& msg) {
        if (!isValidWorldItemRemove(msg)) return false;
        writer.writeByte(msg.type);
        writer.writeUInt32(msg.world_item_id);
        return true;
    }

    inline bool deserialize(Net::BitReader& reader, WorldItemRemoveMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(WorldItemMessageType::WORLD_ITEM_REMOVE)) return false;
        msg.world_item_id = reader.readUInt32();
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8 &&
            isValidWorldItemRemove(msg);
    }

    inline void serialize(Net::BitWriter& writer, const ShootResultMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt16(msg.shooter_client_id);
        writer.writeVector3f(msg.ray_origin);
        writer.writeVector3f(msg.hit_position);
        writer.writeUInt32(msg.hit_entity_id);
        writer.writeByte(msg.weapon_type);
    }

    inline bool deserialize(Net::BitReader& reader, ShootResultMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::SHOOT_RESULT)) return false;
        msg.shooter_client_id = reader.readUInt16();
        msg.ray_origin = reader.readVector3f();
        msg.hit_position = reader.readVector3f();
        msg.hit_entity_id = reader.readUInt32();
        msg.weapon_type = reader.readByte();
        return !reader.hasError();
    }

    inline void serialize(Net::BitWriter& writer, const DamageEventMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt16(msg.attacker_client_id);
        writer.writeUInt16(msg.victim_client_id);
        writer.writeUInt32(static_cast<uint32_t>(msg.damage));
        writer.writeUInt32(static_cast<uint32_t>(msg.health_remaining));
        writer.writeVector3f(msg.hit_position);
    }

    inline bool deserialize(Net::BitReader& reader, DamageEventMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::DAMAGE_EVENT)) return false;
        msg.attacker_client_id = reader.readUInt16();
        msg.victim_client_id = reader.readUInt16();
        msg.damage = static_cast<int32_t>(reader.readUInt32());
        msg.health_remaining = static_cast<int32_t>(reader.readUInt32());
        msg.hit_position = reader.readVector3f();
        return !reader.hasError();
    }

    inline void serialize(Net::BitWriter& writer, const PlayerDiedMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt16(msg.victim_client_id);
        writer.writeUInt16(msg.killer_client_id);
        writer.writeVector3f(msg.death_position);
    }

    inline bool deserialize(Net::BitReader& reader, PlayerDiedMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::PLAYER_DIED)) return false;
        msg.victim_client_id = reader.readUInt16();
        msg.killer_client_id = reader.readUInt16();
        msg.death_position = reader.readVector3f();
        return !reader.hasError();
    }

    inline bool serialize(Net::BitWriter& writer, const CombatEventMessage& msg) {
        if (!isValidCombatEvent(msg)) return false;
        writer.writeByte(msg.type);
        writer.writeUInt16(msg.killer_client_id);
        writer.writeUInt16(msg.victim_client_id);
        writer.writeByte(msg.weapon_type);
        writer.writeByte(msg.flags);
        writer.writeUInt32(msg.death_sequence);
        writer.writeBits(msg.publication_sequence, 64);
        writer.writeByte(static_cast<uint8_t>(msg.killer_kills));
        writer.writeByte(static_cast<uint8_t>(msg.victim_deaths));
        writer.writeByte(msg.match_kill_recorded);
        return true;
    }

    inline bool deserialize(Net::BitReader& reader, CombatEventMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatEventMessageType::COMBAT_EVENT)) return false;
        msg.killer_client_id = reader.readUInt16();
        msg.victim_client_id = reader.readUInt16();
        msg.weapon_type = reader.readByte();
        msg.flags = reader.readByte();
        msg.death_sequence = reader.readUInt32();
        msg.publication_sequence = reader.readBits(64);
        msg.killer_kills = static_cast<int8_t>(reader.readByte());
        msg.victim_deaths = static_cast<int8_t>(reader.readByte());
        msg.match_kill_recorded = reader.readByte();
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8 &&
            isValidCombatEvent(msg);
    }

    inline void serialize(Net::BitWriter& writer, const PlayerRespawnMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.entity_id);
        writer.writeVector3f(msg.spawn_position);
        writer.writeUInt32(static_cast<uint32_t>(msg.health));
    }

    inline bool deserialize(Net::BitReader& reader, PlayerRespawnMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::PLAYER_RESPAWN)) return false;
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        msg.spawn_position = reader.readVector3f();
        msg.health = static_cast<int32_t>(reader.readUInt32());
        return !reader.hasError();
    }

    inline void serialize(Net::BitWriter& writer, const WeaponStateMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt32(static_cast<uint32_t>(msg.ammo));
        writer.writeUInt32(static_cast<uint32_t>(msg.max_ammo));
        writer.writeUInt32(static_cast<uint32_t>(msg.reserve_ammo));
        writer.writeUInt32(static_cast<uint32_t>(msg.max_reserve_ammo));
        writer.writeByte(msg.weapon_type);
        writer.writeByte(msg.reloading);
        writer.writeFloat(msg.fire_cooldown);
        writer.writeFloat(msg.reload_timer);
        writer.writeFloat(msg.spread);
        writer.writeFloat(msg.accuracy_penalty);
        writer.writeFloat(msg.recoil_index);
    }

    inline bool deserialize(Net::BitReader& reader, WeaponStateMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::WEAPON_STATE)) return false;
        msg.ammo = static_cast<int32_t>(reader.readUInt32());
        msg.max_ammo = static_cast<int32_t>(reader.readUInt32());
        msg.reserve_ammo = static_cast<int32_t>(reader.readUInt32());
        msg.max_reserve_ammo = static_cast<int32_t>(reader.readUInt32());
        msg.weapon_type = reader.readByte();
        msg.reloading = reader.readByte();
        msg.fire_cooldown = reader.readFloat();
        msg.reload_timer = reader.readFloat();
        msg.spread = reader.readFloat();
        msg.accuracy_penalty = reader.readFloat();
        msg.recoil_index = reader.readFloat();
        return !reader.hasError();
    }

} // namespace CombatSerializer
namespace CompetitiveSerializer
{
    inline void serialize(Net::BitWriter& writer, const RoundStateMessage& msg) {
        writer.writeByte(msg.type); writer.writeByte(msg.phase);
        writer.writeFloat(msg.phase_time_remaining);
        writer.writeUInt32(msg.phase_deadline_tick); writer.writeUInt32(msg.server_tick);
        writer.writeUInt16(msg.round_number);
        writer.writeByte(msg.wins_attackers); writer.writeByte(msg.wins_defenders);
        writer.writeByte(msg.bomb_planted); writer.writeByte(msg.last_reason); writer.writeByte(msg.last_winner);
    }
    inline bool deserialize(Net::BitReader& reader, RoundStateMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::ROUND_STATE)) return false;
        msg.phase = reader.readByte(); msg.phase_time_remaining = reader.readFloat();
        msg.phase_deadline_tick = reader.readUInt32(); msg.server_tick = reader.readUInt32();
        msg.round_number = reader.readUInt16();
        msg.wins_attackers = reader.readByte(); msg.wins_defenders = reader.readByte();
        msg.bomb_planted = reader.readByte(); msg.last_reason = reader.readByte(); msg.last_winner = reader.readByte();
        return !reader.hasError();
    }
    inline void serialize(Net::BitWriter& writer, const EconomyStateMessage& msg) {
        writer.writeByte(msg.type); writer.writeUInt32(static_cast<uint32_t>(msg.balance)); writer.writeByte(msg.loss_streak);
    }
    inline bool deserialize(Net::BitReader& reader, EconomyStateMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::ECONOMY_STATE)) return false;
        msg.balance = static_cast<int32_t>(reader.readUInt32()); msg.loss_streak = reader.readByte();
        return !reader.hasError();
    }
    inline void serialize(Net::BitWriter& writer, const BuyRequestMessage& msg) {
        writer.writeByte(msg.type); writer.writeUInt32(msg.request_seq); writer.writeUInt16(msg.item_id);
    }
    inline bool deserialize(Net::BitReader& reader, BuyRequestMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::BUY_REQUEST)) return false;
        msg.request_seq = reader.readUInt32(); msg.item_id = reader.readUInt16();
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8;
    }
    inline void serialize(Net::BitWriter& writer, const BuyResultMessage& msg) {
        writer.writeByte(msg.type); writer.writeUInt32(msg.request_seq); writer.writeUInt16(msg.item_id);
        writer.writeByte(msg.result); writer.writeUInt32(static_cast<uint32_t>(msg.balance));
    }
    inline bool deserialize(Net::BitReader& reader, BuyResultMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::BUY_RESULT)) return false;
        msg.request_seq = reader.readUInt32(); msg.item_id = reader.readUInt16();
        msg.result = reader.readByte(); msg.balance = static_cast<int32_t>(reader.readUInt32());
        return !reader.hasError();
    }
    inline void serialize(Net::BitWriter& writer, const TeamSelectMessage& msg) {
        writer.writeByte(msg.type); writer.writeByte(msg.desired_team); writer.writeByte(msg.auto_assign);
    }
    inline bool deserialize(Net::BitReader& reader, TeamSelectMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::TEAM_SELECT)) return false;
        msg.desired_team = reader.readByte(); msg.auto_assign = reader.readByte();
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8;
    }
    inline void serialize(Net::BitWriter& writer, const TeamResultMessage& msg) {
        writer.writeByte(msg.type); writer.writeByte(msg.result); writer.writeByte(msg.team);
    }
    inline bool deserialize(Net::BitReader& reader, TeamResultMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitiveMessageType::TEAM_RESULT)) return false;
        msg.result = reader.readByte(); msg.team = reader.readByte();
        return !reader.hasError();
    }

    inline bool serialize(Net::BitWriter& writer, const CompetitivePresentationSnapshotMessage& msg) {
        if (!isValidCompetitivePresentationSnapshot(msg)) return false;
        writer.writeByte(msg.type);
        writer.writeByte(msg.schema_version);
        writer.writeUInt32(msg.match_instance_id);
        writer.writeUInt32(msg.presentation_revision);
        writer.writeUInt32(msg.source_server_tick);
        writer.writeByte(msg.recipient_team);
        writer.writeUInt32(msg.recipient_stable_player_id);
        writer.writeByte(msg.phase);
        writer.writeUInt32(msg.phase_deadline_tick);
        writer.writeUInt16(msg.round_number);
        writer.writeByte(msg.wins_attackers);
        writer.writeByte(msg.wins_defenders);
        writer.writeByte(msg.last_reason);
        writer.writeByte(msg.last_winner);
        writer.writeByte(msg.objective_state);
        writer.writeByte(msg.objective_site);
        writer.writeUInt32(msg.objective_deadline_tick);
        writer.writeByte(msg.channel_kind);
        writer.writeUInt32(msg.channel_actor_stable_id);
        writer.writeUInt32(msg.channel_deadline_tick);
        writer.writeByte(msg.roster_count);
        for (uint8_t index = 0; index < msg.roster_count; ++index) {
            const auto& row = msg.roster[index];
            writer.writeUInt32(row.stable_player_id);
            writer.writeString(row.display_name.c_str(), kCompetitivePresentationMaxNameChars + 1);
            writer.writeByte(row.team);
            writer.writeByte(row.state);
            writer.writeUInt16(row.kills);
            writer.writeUInt16(row.deaths);
            writer.writeByte(row.is_bot);
            writer.writeByte(row.money_visible);
            writer.writeUInt32(static_cast<uint32_t>(row.money));
            writer.writeUInt16(row.ping_ms);
        }
        writer.writeByte(msg.radar_count);
        for (uint8_t index = 0; index < msg.radar_count; ++index) {
            const auto& record = msg.radar[index];
            writer.writeUInt32(record.stable_record_id);
            writer.writeByte(record.kind);
            writer.writeVector3f(record.position);
        }
        return writer.getByteSize() <= kCompetitivePresentationMaxPayloadBytes;
    }

    inline bool deserialize(Net::BitReader& reader, CompetitivePresentationSnapshotMessage& msg) {
        if (reader.getBufferSize() == 0 || reader.getBufferSize() > kCompetitivePresentationMaxPayloadBytes)
            return false;
        msg = CompetitivePresentationSnapshotMessage{};
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CompetitivePresentationMessageType::SNAPSHOT)) return false;
        msg.schema_version = reader.readByte();
        if (msg.schema_version != kCompetitivePresentationSchemaVersion) return false;
        msg.match_instance_id = reader.readUInt32();
        msg.presentation_revision = reader.readUInt32();
        msg.source_server_tick = reader.readUInt32();
        msg.recipient_team = reader.readByte();
        msg.recipient_stable_player_id = reader.readUInt32();
        msg.phase = reader.readByte();
        msg.phase_deadline_tick = reader.readUInt32();
        msg.round_number = reader.readUInt16();
        msg.wins_attackers = reader.readByte();
        msg.wins_defenders = reader.readByte();
        msg.last_reason = reader.readByte();
        msg.last_winner = reader.readByte();
        msg.objective_state = reader.readByte();
        msg.objective_site = reader.readByte();
        msg.objective_deadline_tick = reader.readUInt32();
        msg.channel_kind = reader.readByte();
        msg.channel_actor_stable_id = reader.readUInt32();
        msg.channel_deadline_tick = reader.readUInt32();
        msg.roster_count = reader.readByte();
        if (msg.roster_count > kCompetitivePresentationMaxRows) return false;
        for (uint8_t index = 0; index < msg.roster_count; ++index) {
            auto& row = msg.roster[index];
            char name[kCompetitivePresentationMaxNameChars + 1]{};
            row.stable_player_id = reader.readUInt32();
            reader.readString(name, sizeof(name));
            row.display_name = name;
            row.team = reader.readByte();
            row.state = reader.readByte();
            row.kills = reader.readUInt16();
            row.deaths = reader.readUInt16();
            row.is_bot = reader.readByte();
            row.money_visible = reader.readByte();
            row.money = static_cast<int32_t>(reader.readUInt32());
            row.ping_ms = reader.readUInt16();
        }
        msg.radar_count = reader.readByte();
        if (msg.radar_count > kCompetitivePresentationMaxRadarRecords) return false;
        for (uint8_t index = 0; index < msg.radar_count; ++index) {
            auto& record = msg.radar[index];
            record.stable_record_id = reader.readUInt32();
            record.kind = reader.readByte();
            record.position = reader.readVector3f();
        }
        return !reader.hasError() && reader.getBitPosition() == reader.getBufferSize() * 8 &&
            isValidCompetitivePresentationSnapshot(msg);
    }
}
