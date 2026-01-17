#pragma once

#include "BitStream.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include <vector>

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
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.protocol_version = reader.readUInt32();
        reader.readString(msg.player_name, 32);
        msg.checksum = reader.readUInt32();
        return true;
    }

    // Serialize ConnectAcceptMessage
    inline void serialize(BitWriter& writer, const ConnectAcceptMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt32(msg.level_hash);
    }

    inline bool deserialize(BitReader& reader, ConnectAcceptMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.client_id = reader.readUInt16();
        msg.server_tick = reader.readUInt32();
        msg.level_hash = reader.readUInt32();
        return true;
    }

    // Serialize ConnectRejectMessage
    inline void serialize(BitWriter& writer, const ConnectRejectMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeString(msg.reason, 64);
    }

    inline bool deserialize(BitReader& reader, ConnectRejectMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        reader.readString(msg.reason, 64);
        return true;
    }

    // Serialize DisconnectMessage
    inline void serialize(BitWriter& writer, const DisconnectMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeString(msg.reason, 64);
    }

    inline bool deserialize(BitReader& reader, DisconnectMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        reader.readString(msg.reason, 64);
        return true;
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
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        msg.position = reader.readVector3f();
        msg.camera_yaw = reader.readFloat();
        return true;
    }

    // Serialize DespawnPlayerMessage
    inline void serialize(BitWriter& writer, const DespawnPlayerMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.entity_id);
    }

    inline bool deserialize(BitReader& reader, DespawnPlayerMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.client_id = reader.readUInt16();
        msg.entity_id = reader.readUInt32();
        return true;
    }

    // Serialize InputCommandMessage
    inline void serialize(BitWriter& writer, const InputCommandMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.client_tick);
        writer.writeUInt32(msg.last_received_tick);
        writer.writeByte(msg.buttons);
        writer.writeFloat(msg.camera_yaw);
        writer.writeFloat(msg.camera_pitch);
        writer.writeFloat(msg.move_forward);
        writer.writeFloat(msg.move_right);
    }

    inline bool deserialize(BitReader& reader, InputCommandMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.client_tick = reader.readUInt32();
        msg.last_received_tick = reader.readUInt32();
        msg.buttons = reader.readByte();
        msg.camera_yaw = reader.readFloat();
        msg.camera_pitch = reader.readFloat();
        msg.move_forward = reader.readFloat();
        msg.move_right = reader.readFloat();
        return true;
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
        }
    }

    inline bool deserialize(BitReader& reader, EntityUpdateData& entity) {
        entity.entity_id = reader.readUInt32();
        entity.flags = reader.readByte();

        // Read conditional data based on flags
        if (entity.flags & ComponentFlags::TRANSFORM) {
            entity.position = reader.readVector3f();
        }
        if (entity.flags & ComponentFlags::VELOCITY) {
            entity.velocity = reader.readVector3f();
        }
        if (entity.flags & ComponentFlags::GROUNDED) {
            entity.grounded = reader.readByte();
        }

        return true;
    }

    // Serialize WorldStateUpdateMessage
    inline void serialize(BitWriter& writer, const WorldStateUpdateMessage& msg, const std::vector<EntityUpdateData>& entities) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt16(static_cast<uint16_t>(entities.size()));

        // Serialize each entity
        for (const auto& entity : entities) {
            serialize(writer, entity);
        }
    }

    inline bool deserialize(BitReader& reader, WorldStateUpdateMessage& msg, std::vector<EntityUpdateData>& entities) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.server_tick = reader.readUInt32();
        uint16_t num_entities = reader.readUInt16();

        entities.clear();
        entities.reserve(num_entities);

        // Deserialize each entity
        for (uint16_t i = 0; i < num_entities; i++) {
            EntityUpdateData entity;
            if (deserialize(reader, entity)) {
                entities.push_back(entity);
            }
        }

        return true;
    }

    // Serialize PingMessage
    inline void serialize(BitWriter& writer, const PingMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.timestamp);
    }

    inline bool deserialize(BitReader& reader, PingMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.timestamp = reader.readUInt32();
        return true;
    }

    // Serialize PongMessage
    inline void serialize(BitWriter& writer, const PongMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.timestamp);
    }

    inline bool deserialize(BitReader& reader, PongMessage& msg) {
        msg.type = static_cast<MessageType>(reader.readByte());
        msg.timestamp = reader.readUInt32();
        return true;
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
}
