#pragma once

#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"

#include <cstdint>
#include <glm/glm.hpp>

enum class CombatMessageType : uint8_t
{
    SHOOT_RESULT = Net::CUSTOM_MESSAGE_START,
    DAMAGE_EVENT = Net::CUSTOM_MESSAGE_START + 1,
    PLAYER_DIED = Net::CUSTOM_MESSAGE_START + 2,
    PLAYER_RESPAWN = Net::CUSTOM_MESSAGE_START + 3,
    WEAPON_STATE = Net::CUSTOM_MESSAGE_START + 4
};

// Competitive mode messages (wire v3 custom messages >= CUSTOM_MESSAGE_START+5 = 69).
enum class CompetitiveMessageType : uint8_t
{
    ROUND_STATE = Net::CUSTOM_MESSAGE_START + 5,    // 69 S->C reliable
    ECONOMY_STATE = Net::CUSTOM_MESSAGE_START + 6,  // 70 S->C owner-only reliable
    BUY_REQUEST = Net::CUSTOM_MESSAGE_START + 7,    // 71 C->S reliable
    BUY_RESULT = Net::CUSTOM_MESSAGE_START + 8,     // 72 S->C reliable
    TEAM_SELECT = Net::CUSTOM_MESSAGE_START + 9,    // 73 C->S reliable
    TEAM_RESULT = Net::CUSTOM_MESSAGE_START + 10    // 74 S->C reliable
};

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
        return !reader.hasError();
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
        return !reader.hasError();
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
}
