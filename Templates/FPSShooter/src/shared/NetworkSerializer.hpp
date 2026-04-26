#pragma once

#include "BitStream.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include <vector>
#include <cstring>
#include <utility>

// Namespace for all serialization functions
namespace NetworkSerializer
{
    // Serialize ConnectRequestMessage
    inline void serialize(BitWriter& writer, const ConnectRequestMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.protocol_version);
        writer.writeString(msg.player_name, 32);
        writer.writeUInt32(msg.checksum);
    }

    inline bool deserialize(BitReader& reader, ConnectRequestMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_REQUEST) return false;
        msg.protocol_version = reader.readUInt32();
        reader.readString(msg.player_name, 32);
        msg.checksum = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize ConnectAcceptMessage
    inline void serialize(BitWriter& writer, const ConnectAcceptMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt32(msg.level_hash);
    }

    inline bool deserialize(BitReader& reader, ConnectAcceptMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_ACCEPT) return false;
        msg.client_id = reader.readUInt16();
        msg.server_tick = reader.readUInt32();
        msg.level_hash = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize ConnectRejectMessage
    inline void serialize(BitWriter& writer, const ConnectRejectMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeString(msg.reason, 64);
    }

    inline bool deserialize(BitReader& reader, ConnectRejectMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_REJECT) return false;
        reader.readString(msg.reason, 64);
        return !reader.hasError();
    }

    // Serialize DisconnectMessage
    inline void serialize(BitWriter& writer, const DisconnectMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeString(msg.reason, 64);
    }

    inline bool deserialize(BitReader& reader, DisconnectMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::DISCONNECT) return false;
        reader.readString(msg.reason, 64);
        return !reader.hasError();
    }

    // Serialize SpawnPlayerMessage
    inline void serialize(BitWriter& writer, const SpawnPlayerMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.entity_id);
        writer.writeVector3f(msg.position);
        writer.writeFloat(msg.camera_yaw);
    }

    inline bool deserialize(BitReader& reader, SpawnPlayerMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::SPAWN_PLAYER) return false;
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        msg.position = reader.readVector3f();
        msg.camera_yaw = reader.readFloat();
        return !reader.hasError();
    }

    // Serialize DespawnPlayerMessage
    inline void serialize(BitWriter& writer, const DespawnPlayerMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.entity_id);
    }

    inline bool deserialize(BitReader& reader, DespawnPlayerMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::DESPAWN_PLAYER) return false;
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize InputCommandMessage (with redundant inputs for packet loss resilience)
    inline void serialize(BitWriter& writer, const InputCommandMessage& msg,
                          const InputSample* redundant_inputs = nullptr, uint8_t redundant_count = 0) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.client_tick);
        writer.writeUInt32(msg.last_received_tick);
        uint8_t total_count = 1 + redundant_count; // Primary + redundant
        writer.writeByte(total_count);
        // Primary input
        writer.writeByte(msg.buttons);
        writer.writeFloat(msg.camera_yaw);
        writer.writeFloat(msg.camera_pitch);
        writer.writeFloat(msg.move_forward);
        writer.writeFloat(msg.move_right);
        // Redundant older inputs
        for (uint8_t i = 0; i < redundant_count; i++) {
            writer.writeUInt32(redundant_inputs[i].tick);
            writer.writeByte(redundant_inputs[i].buttons);
            writer.writeFloat(redundant_inputs[i].camera_yaw);
            writer.writeFloat(redundant_inputs[i].camera_pitch);
            writer.writeFloat(redundant_inputs[i].move_forward);
            writer.writeFloat(redundant_inputs[i].move_right);
        }
    }

    inline bool deserialize(BitReader& reader, InputCommandMessage& msg,
                            std::vector<InputSample>& redundant_inputs) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::INPUT_COMMAND) return false;
        msg.client_tick = reader.readUInt32();
        msg.last_received_tick = reader.readUInt32();
        msg.input_count = reader.readByte();
        if (msg.input_count == 0 || msg.input_count > 4) return false;
        // Primary input
        msg.buttons = reader.readByte();
        msg.camera_yaw = reader.readFloat();
        msg.camera_pitch = reader.readFloat();
        msg.move_forward = reader.readFloat();
        msg.move_right = reader.readFloat();
        // Redundant older inputs
        redundant_inputs.clear();
        for (uint8_t i = 1; i < msg.input_count; i++) {
            InputSample sample;
            sample.tick = reader.readUInt32();
            sample.buttons = reader.readByte();
            sample.camera_yaw = reader.readFloat();
            sample.camera_pitch = reader.readFloat();
            sample.move_forward = reader.readFloat();
            sample.move_right = reader.readFloat();
            redundant_inputs.push_back(sample);
        }
        return !reader.hasError();
    }

    // Serialize EntityUpdateData (used within WorldStateUpdateMessage)
    inline void serialize(BitWriter& writer, const EntityUpdateData& entity) {
        writer.writeUInt32(entity.entity_id);
        writer.writeByte(entity.flags);

        // Write conditional data based on flags
        if (entity.flags & ComponentFlags::TRANSFORM) {
            writer.writeVector3f(entity.position);
        }
        if (entity.flags & ComponentFlags::VELOCITY) {
            writer.writeVector3f(entity.velocity);
        }
        if (entity.flags & ComponentFlags::GROUNDED) {
            writer.writeByte(entity.grounded);
            writer.writeVector3f(entity.ground_normal);
        }
        if (entity.flags & ComponentFlags::ROTATION) {
            writer.writeFloat(entity.rotation_y);
        }
    }

    inline bool deserialize(BitReader& reader, EntityUpdateData& entity) {
        // Zero-init so a partial read isn't observable as garbage if caller forgets to check.
        entity = EntityUpdateData{};

        if (!reader.canRead(40)) return false;  // entity_id(32) + flags(8)
        entity.entity_id = reader.readUInt32();
        entity.flags = reader.readByte();

        // Pre-validate the conditional body fits in the remaining buffer.
        size_t body_bits = 0;
        if (entity.flags & ComponentFlags::TRANSFORM) body_bits += 96;            // 3 x float
        if (entity.flags & ComponentFlags::VELOCITY)  body_bits += 96;            // 3 x float
        if (entity.flags & ComponentFlags::GROUNDED)  body_bits += 8 + 96;        // byte + 3 x float
        if (entity.flags & ComponentFlags::ROTATION)  body_bits += 32;            // 1 x float
        if (body_bits > 0 && !reader.canRead(body_bits)) return false;

        // Read conditional data based on flags
        if (entity.flags & ComponentFlags::TRANSFORM) {
            entity.position = reader.readVector3f();
        }
        if (entity.flags & ComponentFlags::VELOCITY) {
            entity.velocity = reader.readVector3f();
        }
        if (entity.flags & ComponentFlags::GROUNDED) {
            entity.grounded = reader.readByte();
            entity.ground_normal = reader.readVector3f();
        }
        if (entity.flags & ComponentFlags::ROTATION) {
            entity.rotation_y = reader.readFloat();
        }

        return !reader.hasError();
    }

    // Serialize WorldStateUpdateMessage
    inline void serialize(BitWriter& writer, const WorldStateUpdateMessage& msg, const std::vector<EntityUpdateData>& entities) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt16(static_cast<uint16_t>(entities.size()));
        writer.writeUInt32(msg.last_processed_input_tick);

        // Serialize each entity
        for (const auto& entity : entities) {
            serialize(writer, entity);
        }
    }

    inline bool deserialize(BitReader& reader, WorldStateUpdateMessage& msg, std::vector<EntityUpdateData>& entities) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::WORLD_STATE_UPDATE) return false;
        msg.server_tick = reader.readUInt32();
        uint16_t num_entities = reader.readUInt16();
        if (num_entities > MAX_NETWORKED_ENTITIES) return false;
        msg.last_processed_input_tick = reader.readUInt32();

        entities.clear();
        entities.reserve(num_entities);

        // Deserialize each entity
        for (uint16_t i = 0; i < num_entities; i++) {
            EntityUpdateData entity;
            if (!deserialize(reader, entity)) {
                return false;  // Stop on first error
            }
            entities.push_back(entity);
        }

        return !reader.hasError();
    }

    // Serialize PingMessage
    inline void serialize(BitWriter& writer, const PingMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.timestamp);
    }

    inline bool deserialize(BitReader& reader, PingMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::PING) return false;
        msg.timestamp = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize PongMessage
    inline void serialize(BitWriter& writer, const PongMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.timestamp);
    }

    inline bool deserialize(BitReader& reader, PongMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::PONG) return false;
        msg.timestamp = reader.readUInt32();
        return !reader.hasError();
    }

    // Helper: Get message type from raw data (peek first byte)
    inline MessageType getMessageType(const uint8_t* data, size_t size) {
        if (size < 1) return MessageType::DISCONNECT;
        return static_cast<MessageType>(data[0]);
    }

    // Helper: Create an ENet packet from serialized data
    inline ENetPacket* createPacket(const BitWriter& writer, bool reliable) {
        ENetPacketFlag flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
        return enet_packet_create(writer.getData(), writer.getByteSize(), flags);
    }

    // Serialize ShootCommandMessage
    inline void serialize(BitWriter& writer, const ShootCommandMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.client_tick);
        writer.writeVector3f(msg.ray_origin);
        writer.writeVector3f(msg.ray_direction);
        writer.writeByte(msg.weapon_type);
    }

    inline bool deserialize(BitReader& reader, ShootCommandMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::SHOOT_COMMAND) return false;
        msg.client_tick = reader.readUInt32();
        msg.ray_origin = reader.readVector3f();
        msg.ray_direction = reader.readVector3f();
        msg.weapon_type = reader.readByte();
        return !reader.hasError();
    }

    // Serialize ShootResultMessage
    inline void serialize(BitWriter& writer, const ShootResultMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.shooter_client_id);
        writer.writeVector3f(msg.ray_origin);
        writer.writeVector3f(msg.hit_position);
        writer.writeUInt32(msg.hit_entity_id);
        writer.writeByte(msg.weapon_type);
    }

    inline bool deserialize(BitReader& reader, ShootResultMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::SHOOT_RESULT) return false;
        msg.shooter_client_id = reader.readUInt16();
        msg.ray_origin = reader.readVector3f();
        msg.hit_position = reader.readVector3f();
        msg.hit_entity_id = reader.readUInt32();
        msg.weapon_type = reader.readByte();
        return !reader.hasError();
    }

    // Serialize DamageEventMessage
    inline void serialize(BitWriter& writer, const DamageEventMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.attacker_client_id);
        writer.writeUInt16(msg.victim_client_id);
        writer.writeUInt32(static_cast<uint32_t>(msg.damage));
        writer.writeUInt32(static_cast<uint32_t>(msg.health_remaining));
        writer.writeVector3f(msg.hit_position);
    }

    inline bool deserialize(BitReader& reader, DamageEventMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::DAMAGE_EVENT) return false;
        msg.attacker_client_id = reader.readUInt16();
        msg.victim_client_id = reader.readUInt16();
        msg.damage = static_cast<int32_t>(reader.readUInt32());
        msg.health_remaining = static_cast<int32_t>(reader.readUInt32());
        msg.hit_position = reader.readVector3f();
        return !reader.hasError();
    }

    // Serialize PlayerDiedMessage
    inline void serialize(BitWriter& writer, const PlayerDiedMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.victim_client_id);
        writer.writeUInt16(msg.killer_client_id);
        writer.writeVector3f(msg.death_position);
    }

    inline bool deserialize(BitReader& reader, PlayerDiedMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::PLAYER_DIED) return false;
        msg.victim_client_id = reader.readUInt16();
        msg.killer_client_id = reader.readUInt16();
        msg.death_position = reader.readVector3f();
        return !reader.hasError();
    }

    // Serialize PlayerRespawnMessage
    inline void serialize(BitWriter& writer, const PlayerRespawnMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.entity_id);
        writer.writeVector3f(msg.spawn_position);
        writer.writeUInt32(static_cast<uint32_t>(msg.health));
    }

    inline bool deserialize(BitReader& reader, PlayerRespawnMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::PLAYER_RESPAWN) return false;
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        msg.spawn_position = reader.readVector3f();
        msg.health = static_cast<int32_t>(reader.readUInt32());
        return !reader.hasError();
    }

    // Serialize WeaponStateMessage
    inline void serialize(BitWriter& writer, const WeaponStateMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(static_cast<uint32_t>(msg.ammo));
        writer.writeUInt32(static_cast<uint32_t>(msg.max_ammo));
        writer.writeByte(msg.weapon_type);
        writer.writeByte(msg.reloading);
    }

    inline bool deserialize(BitReader& reader, WeaponStateMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::WEAPON_STATE) return false;
        msg.ammo = static_cast<int32_t>(reader.readUInt32());
        msg.max_ammo = static_cast<int32_t>(reader.readUInt32());
        msg.weapon_type = reader.readByte();
        msg.reloading = reader.readByte();
        return !reader.hasError();
    }

    // Serialize CVarSyncMessage
    inline void serialize(BitWriter& writer, const CVarSyncMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeString(msg.cvar_name, 64);
        writer.writeString(msg.cvar_value, 128);
    }

    inline bool deserialize(BitReader& reader, CVarSyncMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CVAR_SYNC) return false;
        reader.readString(msg.cvar_name, 64);
        reader.readString(msg.cvar_value, 128);
        return !reader.hasError();
    }

    // Serialize CVarInitialSyncMessage with cvar data
    inline void serialize(BitWriter& writer, const CVarInitialSyncMessage& msg,
                          const std::vector<std::pair<std::string, std::string>>& cvars) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(static_cast<uint16_t>(cvars.size()));
        for (const auto& [name, value] : cvars) {
            char nameBuf[64] = {0};
            char valueBuf[128] = {0};
            std::strncpy(nameBuf, name.c_str(), 63);
            std::strncpy(valueBuf, value.c_str(), 127);
            writer.writeString(nameBuf, 64);
            writer.writeString(valueBuf, 128);
        }
    }

    inline bool deserialize(BitReader& reader, CVarInitialSyncMessage& msg,
                            std::vector<std::pair<std::string, std::string>>& cvars) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CVAR_INITIAL_SYNC) return false;
        msg.cvar_count = reader.readUInt16();
        if (msg.cvar_count > MAX_SYNCED_CVARS) return false;

        cvars.clear();
        cvars.reserve(msg.cvar_count);
        for (uint16_t i = 0; i < msg.cvar_count; i++) {
            char name[64] = {0};
            char value[128] = {0};
            reader.readString(name, 64);
            reader.readString(value, 128);
            // Bail before pushing partial/garbage data on truncated wire input.
            if (reader.hasError()) {
                cvars.clear();
                return false;
            }
            cvars.emplace_back(name, value);
        }
        return !reader.hasError();
    }
}
