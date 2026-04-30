#pragma once

#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"

#include <cstdint>
#include <glm/glm.hpp>

enum class CombatMessageType : uint8_t
{
    SHOOT_COMMAND = Net::CUSTOM_MESSAGE_START,
    SHOOT_RESULT = Net::CUSTOM_MESSAGE_START + 1,
    DAMAGE_EVENT = Net::CUSTOM_MESSAGE_START + 2,
    PLAYER_DIED = Net::CUSTOM_MESSAGE_START + 3,
    PLAYER_RESPAWN = Net::CUSTOM_MESSAGE_START + 4,
    WEAPON_STATE = Net::CUSTOM_MESSAGE_START + 5
};

struct ShootCommandMessage
{
    uint8_t type = static_cast<uint8_t>(CombatMessageType::SHOOT_COMMAND);
    uint32_t client_tick = 0;
    glm::vec3 ray_origin = glm::vec3(0);
    glm::vec3 ray_direction = glm::vec3(0, 0, -1);
    uint8_t weapon_type = 0;
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
    uint8_t weapon_type = 0;
    uint8_t reloading = 0;
};

namespace CombatSerializer
{
    inline void serialize(Net::BitWriter& writer, const ShootCommandMessage& msg) {
        writer.writeByte(msg.type);
        writer.writeUInt32(msg.client_tick);
        writer.writeVector3f(msg.ray_origin);
        writer.writeVector3f(msg.ray_direction);
        writer.writeByte(msg.weapon_type);
    }

    inline bool deserialize(Net::BitReader& reader, ShootCommandMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::SHOOT_COMMAND)) return false;
        msg.client_tick = reader.readUInt32();
        msg.ray_origin = reader.readVector3f();
        msg.ray_direction = reader.readVector3f();
        msg.weapon_type = reader.readByte();
        return !reader.hasError();
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
        writer.writeByte(msg.weapon_type);
        writer.writeByte(msg.reloading);
    }

    inline bool deserialize(Net::BitReader& reader, WeaponStateMessage& msg) {
        msg.type = reader.readByte();
        if (msg.type != static_cast<uint8_t>(CombatMessageType::WEAPON_STATE)) return false;
        msg.ammo = static_cast<int32_t>(reader.readUInt32());
        msg.max_ammo = static_cast<int32_t>(reader.readUInt32());
        msg.weapon_type = reader.readByte();
        msg.reloading = reader.readByte();
        return !reader.hasError();
    }
}
