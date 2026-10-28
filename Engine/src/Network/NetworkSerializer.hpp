#pragma once

#include "BitStream.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include "NetworkInput.hpp"
#include "ReplicationRegistry.hpp"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Net {

// Namespace for all serialization functions
namespace NetworkSerializer
{
    inline bool isFinite(float value)
    {
        return std::isfinite(value);
    }

    inline bool isFinite(const glm::vec3& value)
    {
        return isFinite(value.x) && isFinite(value.y) && isFinite(value.z);
    }

    inline bool hasOnlyKnownComponentFlags(uint8_t flags)
    {
        return (flags & ~ComponentFlags::ALL_KNOWN) == 0;
    }

    inline bool hasOnlyKnownSnapshotFlags(uint8_t flags)
    {
        return (flags & ~SnapshotFlags::ALL_KNOWN) == 0;
    }

    inline bool tryGetMessageType(const uint8_t* data, size_t size, uint8_t& out_type)
    {
        if (data == nullptr || size < 1) {
            return false;
        }
        out_type = data[0];
        return true;
    }

    // Peek the protocol version of a CONNECT_REQUEST without deserialising
    // the rest (v2 packets are shorter; the version gate must fire first).
    inline bool tryPeekProtocolVersion(const uint8_t* data, size_t size, uint32_t& out_version)
    {
        if (data == nullptr || size < 5 || data[0] != static_cast<uint8_t>(MessageType::CONNECT_REQUEST)) {
            return false;
        }
        std::memcpy(&out_version, data + 1, sizeof(uint32_t));
        return true;
    }

    // Serialize ConnectRequestMessage (v4: level_hash + capability_flags,
    // the v2 checksum field is deleted)
    inline void serialize(BitWriter& writer, const ConnectRequestMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.protocol_version);
        writer.writeString(msg.player_name, 32);
        writer.writeUInt32(msg.level_hash);
        writer.writeUInt32(msg.capability_flags);
    }

    inline bool deserialize(BitReader& reader, ConnectRequestMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_REQUEST) return false;
        msg.protocol_version = reader.readUInt32();
        reader.readString(msg.player_name, 32);
        if (reader.hasError() || !reader.canRead(64)) return false;
        msg.level_hash = reader.readUInt32();
        msg.capability_flags = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize ConnectAcceptMessage
    inline void serialize(BitWriter& writer, const ConnectAcceptMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt16(msg.client_id);
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt32(msg.level_hash);
        writer.writeUInt32(msg.capability_flags);
    }

    inline bool deserialize(BitReader& reader, ConnectAcceptMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_ACCEPT) return false;
        msg.client_id = reader.readUInt16();
        msg.server_tick = reader.readUInt32();
        msg.level_hash = reader.readUInt32();
        if (reader.hasError() || !reader.canRead(32)) return false;
        msg.capability_flags = reader.readUInt32();
        return !reader.hasError();
    }

    // Serialize ConnectRejectMessage (v4: reject_code byte + reason)
    inline void serialize(BitWriter& writer, const ConnectRejectMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeByte(msg.reject_code);
        writer.writeString(msg.reason, 64);
    }

    inline bool deserialize(BitReader& reader, ConnectRejectMessage& msg) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::CONNECT_REJECT) return false;
        msg.reject_code = reader.readByte();
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

    // Serialize SpawnPlayerMessage (camera_yaw is radians on the wire, D4)
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
        if (!isFinite(msg.position) || !isFinite(msg.camera_yaw)) return false;
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

    // ---- Generic replication extension block (D2) ----
    // Block body (ext_len bytes): u16 component_count, then per entry
    // u16 component_id | u16 field_mask | u16 entry_len | payload[entry_len].

    inline void writeExtEntries(BitWriter& writer, const std::vector<ReplicatedExtEntry>& entries) {
        writer.writeUInt16(static_cast<uint16_t>(entries.size()));
        for (const auto& entry : entries) {
            writer.writeUInt16(entry.component_id);
            writer.writeUInt16(entry.field_mask);
            writer.writeUInt16(static_cast<uint16_t>(entry.payload.size()));
            for (uint8_t byte : entry.payload) writer.writeByte(byte);
        }
    }

    inline size_t extEntriesByteSize(const std::vector<ReplicatedExtEntry>& entries) {
        size_t size = 2;
        for (const auto& entry : entries) size += 6 + entry.payload.size();
        return size;
    }

    // Reads one ext block body of ext_len bytes at the reader's position.
    // Returns false only on structural overflow (whole message rejected).
    // Unknown components are skipped; unknown field_mask bits reject only
    // this entity's extension (counted, entries cleared).
    inline bool readExtBlock(BitReader& reader, uint16_t ext_len, std::vector<ReplicatedExtEntry>& out_entries) {
        out_entries.clear();
        if (ext_len == 0) return true;
        if (!reader.canRead(static_cast<size_t>(ext_len) * 8)) return false;
        const size_t block_end_bits = reader.getBitPosition() + static_cast<size_t>(ext_len) * 8;
        if (!reader.canRead(16)) return false;
        const uint16_t component_count = reader.readUInt16();
        bool entity_ext_rejected = false;
        for (uint16_t i = 0; i < component_count; ++i) {
            if (reader.getBitPosition() + 48 > block_end_bits) return false;
            const uint16_t component_id = reader.readUInt16();
            const uint16_t field_mask = reader.readUInt16();
            const uint16_t entry_len = reader.readUInt16();
            if (reader.getBitPosition() + static_cast<size_t>(entry_len) * 8 > block_end_bits) return false;
            std::vector<uint8_t> payload;
            payload.reserve(entry_len);
            for (uint16_t b = 0; b < entry_len; ++b) payload.push_back(reader.readByte());
            if (reader.hasError()) return false;
            uint16_t field_count = 0;
            if (!ReplicationRegistry::get().getFieldCount(component_id, field_count)) {
                continue; // unknown component: skipped via entry_len
            }
            if (field_count < 16 && (field_mask >> field_count) != 0) {
                ReplicationRegistry::get().ext_field_mask_reject_count++;
                entity_ext_rejected = true;
                continue;
            }
            if (!entity_ext_rejected) {
                out_entries.push_back(ReplicatedExtEntry{component_id, field_mask, std::move(payload)});
            }
        }
        if (reader.getBitPosition() != block_end_bits) {
            reader.seekBits(block_end_bits); // tolerate trailing padding in block
        }
        if (entity_ext_rejected) out_entries.clear();
        return !reader.hasError();
    }

    // Serialize SpawnEntityMessage (SPAWN_ENTITY = 6, reliable)
    inline void serialize(BitWriter& writer, const SpawnEntityMessage& msg) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.network_id);
        writer.writeUInt16(msg.archetype_id);
        writer.writeUInt16(msg.owner_client_id);
        writer.writeVector3f(msg.position);
        writer.writeFloat(msg.rotation_y);
        writer.writeUInt16(static_cast<uint16_t>(msg.init_ext.size()));
        for (uint8_t byte : msg.init_ext) writer.writeByte(byte);
    }

    inline bool deserialize(BitReader& reader, SpawnEntityMessage& msg) {
        msg = SpawnEntityMessage{};
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::SPAWN_ENTITY) return false;
        msg.network_id = reader.readUInt32();
        if (msg.network_id == 0) return false;
        msg.archetype_id = reader.readUInt16();
        msg.owner_client_id = reader.readUInt16();
        msg.position = reader.readVector3f();
        msg.rotation_y = reader.readFloat();
        if (!isFinite(msg.position) || !isFinite(msg.rotation_y)) return false;
        if (reader.hasError() || !reader.canRead(16)) return false;
        const uint16_t init_ext_len = reader.readUInt16();
        if (!reader.canRead(static_cast<size_t>(init_ext_len) * 8)) return false;
        msg.init_ext.clear();
        msg.init_ext.reserve(init_ext_len);
        for (uint16_t i = 0; i < init_ext_len; ++i) msg.init_ext.push_back(reader.readByte());
        return !reader.hasError();
    }

    // Serialize InputCommandMessage (uint16 buttons; view tick after
    // move_right per sample, only when CAP_VIEW_TICK was negotiated)
    inline void serialize(BitWriter& writer, const InputCommandMessage& msg,
                          const InputSample* redundant_inputs, uint8_t redundant_count,
                          bool include_view_tick) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.client_tick);
        writer.writeUInt32(msg.last_received_tick);
        if (redundant_inputs == nullptr) {
            redundant_count = 0;
        }
        uint8_t total_count = static_cast<uint8_t>(1 + (std::min<uint8_t>)(redundant_count, MAX_INPUT_SAMPLES_PER_PACKET - 1));
        writer.writeByte(total_count);
        // Primary input
        writer.writeUInt16(msg.buttons);
        writer.writeFloat(msg.camera_yaw);
        writer.writeFloat(msg.camera_pitch);
        writer.writeFloat(msg.move_forward);
        writer.writeFloat(msg.move_right);
        if (include_view_tick) writer.writeUInt32(msg.view_server_tick);
        // Redundant older inputs
        for (uint8_t i = 0; i < total_count - 1; i++) {
            writer.writeUInt32(redundant_inputs[i].tick);
            writer.writeUInt16(redundant_inputs[i].buttons);
            writer.writeFloat(redundant_inputs[i].camera_yaw);
            writer.writeFloat(redundant_inputs[i].camera_pitch);
            writer.writeFloat(redundant_inputs[i].move_forward);
            writer.writeFloat(redundant_inputs[i].move_right);
            if (include_view_tick) writer.writeUInt32(redundant_inputs[i].view_server_tick);
        }
    }

    inline void serialize(BitWriter& writer, const InputCommandMessage& msg,
                          const InputSample* redundant_inputs = nullptr, uint8_t redundant_count = 0) {
        serialize(writer, msg, redundant_inputs, redundant_count, true);
    }

    inline bool deserialize(BitReader& reader, InputCommandMessage& msg,
                            std::vector<InputSample>& redundant_inputs, bool include_view_tick) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::INPUT_COMMAND) return false;
        msg.client_tick = reader.readUInt32();
        msg.last_received_tick = reader.readUInt32();
        msg.input_count = reader.readByte();
        if (msg.input_count == 0 || msg.input_count > MAX_INPUT_SAMPLES_PER_PACKET) return false;
        // Primary input
        msg.buttons = sanitizeWeaponSelect(reader.readUInt16(), &ReplicationRegistry::get().weapon_select_reserved_count);
        msg.camera_yaw = reader.readFloat();
        msg.camera_pitch = reader.readFloat();
        msg.move_forward = reader.readFloat();
        msg.move_right = reader.readFloat();
        msg.view_server_tick = include_view_tick ? reader.readUInt32() : msg.last_received_tick;
        if (!isFinite(msg.camera_yaw) ||
            !isFinite(msg.camera_pitch) ||
            !isFinite(msg.move_forward) ||
            !isFinite(msg.move_right)) {
            return false;
        }
        // Redundant older inputs
        redundant_inputs.clear();
        for (uint8_t i = 1; i < msg.input_count; i++) {
            InputSample sample;
            sample.tick = reader.readUInt32();
            sample.buttons = sanitizeWeaponSelect(reader.readUInt16(), &ReplicationRegistry::get().weapon_select_reserved_count);
            sample.camera_yaw = reader.readFloat();
            sample.camera_pitch = reader.readFloat();
            sample.move_forward = reader.readFloat();
            sample.move_right = reader.readFloat();
            sample.view_server_tick = include_view_tick ? reader.readUInt32() : msg.last_received_tick;
            if (!isFinite(sample.camera_yaw) ||
                !isFinite(sample.camera_pitch) ||
                !isFinite(sample.move_forward) ||
                !isFinite(sample.move_right)) {
                redundant_inputs.clear();
                return false;
            }
            redundant_inputs.push_back(sample);
        }
        return !reader.hasError();
    }

    inline bool deserialize(BitReader& reader, InputCommandMessage& msg,
                            std::vector<InputSample>& redundant_inputs) {
        return deserialize(reader, msg, redundant_inputs, true);
    }

    // Serialize EntityUpdateData built-in fields (used within WorldStateUpdateMessage).
    // Extension entries are written by the world-state serializer under HAS_EXT.
    inline void serialize(BitWriter& writer, const EntityUpdateData& entity) {
        writer.writeUInt32(entity.entity_id);
        writer.writeByte(entity.flags);

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
        entity = EntityUpdateData{};

        if (!reader.canRead(40)) return false;  // entity_id(32) + flags(8)
        entity.entity_id = reader.readUInt32();
        entity.flags = reader.readByte();
        if (entity.entity_id == 0) return false;
        if (!hasOnlyKnownComponentFlags(entity.flags)) return false;
        if ((entity.flags & ComponentFlags::DELETED) != 0 && entity.flags != ComponentFlags::DELETED) return false;

        size_t body_bits = 0;
        if (entity.flags & ComponentFlags::TRANSFORM) body_bits += 96;
        if (entity.flags & ComponentFlags::VELOCITY)  body_bits += 96;
        if (entity.flags & ComponentFlags::GROUNDED)  body_bits += 8 + 96;
        if (entity.flags & ComponentFlags::ROTATION)  body_bits += 32;
        if (body_bits > 0 && !reader.canRead(body_bits)) return false;

        if (entity.flags & ComponentFlags::TRANSFORM) {
            entity.position = reader.readVector3f();
            if (!isFinite(entity.position)) return false;
        }
        if (entity.flags & ComponentFlags::VELOCITY) {
            entity.velocity = reader.readVector3f();
            if (!isFinite(entity.velocity)) return false;
        }
        if (entity.flags & ComponentFlags::GROUNDED) {
            entity.grounded = reader.readByte();
            if (entity.grounded > 1) return false;
            entity.ground_normal = reader.readVector3f();
            if (!isFinite(entity.ground_normal)) return false;
        }
        if (entity.flags & ComponentFlags::ROTATION) {
            entity.rotation_y = reader.readFloat();
            if (!isFinite(entity.rotation_y)) return false;
        }

        return !reader.hasError();
    }

    // Serialize WorldStateUpdateMessage. Under HAS_EXT each entity is followed
    // by u16 ext_len + ext block (ext_len 0 when the entity has no entries).
    inline void serialize(BitWriter& writer, const WorldStateUpdateMessage& msg, const std::vector<EntityUpdateData>& entities) {
        writer.writeByte(static_cast<uint8_t>(msg.type));
        writer.writeUInt32(msg.server_tick);
        writer.writeUInt32(msg.delta_from_tick);
        writer.writeByte(msg.snapshot_flags);
        writer.writeUInt16(static_cast<uint16_t>(entities.size()));
        writer.writeUInt32(msg.last_processed_input_tick);

        const bool has_ext = (msg.snapshot_flags & SnapshotFlags::HAS_EXT) != 0;
        for (const auto& entity : entities) {
            serialize(writer, entity);
            if (has_ext) {
                if (entity.ext_entries.empty()) {
                    writer.writeUInt16(0);
                } else {
                    writer.writeUInt16(static_cast<uint16_t>(extEntriesByteSize(entity.ext_entries)));
                    writeExtEntries(writer, entity.ext_entries);
                }
            }
        }
    }

    inline bool deserialize(BitReader& reader, WorldStateUpdateMessage& msg, std::vector<EntityUpdateData>& entities) {
        if (!reader.canRead(8)) return false;
        msg.type = static_cast<MessageType>(reader.readByte());
        if (msg.type != MessageType::WORLD_STATE_UPDATE) return false;
        msg.server_tick = reader.readUInt32();
        msg.delta_from_tick = reader.readUInt32();
        msg.snapshot_flags = reader.readByte();
        if (!hasOnlyKnownSnapshotFlags(msg.snapshot_flags)) return false;
        if (msg.isFullSnapshot() && msg.delta_from_tick != 0) return false;
        uint16_t num_entities = reader.readUInt16();
        if (num_entities > MAX_NETWORKED_ENTITIES) return false;
        msg.num_entities = num_entities;
        msg.last_processed_input_tick = reader.readUInt32();

        entities.clear();
        entities.reserve(num_entities);

        const bool has_ext = (msg.snapshot_flags & SnapshotFlags::HAS_EXT) != 0;
        for (uint16_t i = 0; i < num_entities; i++) {
            EntityUpdateData entity;
            if (!deserialize(reader, entity)) {
                entities.clear();
                return false;
            }
            if (has_ext) {
                if (!reader.canRead(16)) { entities.clear(); return false; }
                const uint16_t ext_len = reader.readUInt16();
                if (!readExtBlock(reader, ext_len, entity.ext_entries)) {
                    entities.clear();
                    return false;
                }
            }
            entities.push_back(std::move(entity));
        }

        if (reader.hasError()) { entities.clear(); return false; }
        return true;
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
    inline uint8_t getMessageType(const uint8_t* data, size_t size) {
        uint8_t type = static_cast<uint8_t>(MessageType::DISCONNECT);
        tryGetMessageType(data, size, type);
        return type;
    }

    // Helper: Create an ENet packet from serialized data
    inline ENetPacket* createPacket(const BitWriter& writer, bool reliable) {
        ENetPacketFlag flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
        return enet_packet_create(writer.getData(), writer.getByteSize(), flags);
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
            if (reader.hasError()) {
                cvars.clear();
                return false;
            }
            cvars.emplace_back(name, value);
        }
        return !reader.hasError();
    }
} // namespace NetworkSerializer

} // namespace Net
