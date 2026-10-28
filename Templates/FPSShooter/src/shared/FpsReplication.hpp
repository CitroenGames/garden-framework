#pragma once

#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/ReplicationRegistry.hpp"
#include "SharedComponents.hpp"

#include <cstdint>
#include <vector>

namespace FPSShooter::Replication {

// Stable FPSShooter protocol id. This is template schema, never an EngineCore
// type, and all fields are current-version-only.
inline constexpr uint16_t FpsHealthComponentId = 0x1001;
inline constexpr uint16_t FpsHealthFields = 0x0007;

inline void writeInt32(Net::BitWriter& writer, int32_t value)
{
    writer.writeUInt32(static_cast<uint32_t>(value));
}

inline bool readHealthPayload(const Net::ReplicatedExtEntry& entry, FpsHealthComponent& out)
{
    if (entry.component_id != FpsHealthComponentId || entry.field_mask != FpsHealthFields ||
        entry.payload.size() != 9u) {
        return false;
    }

    Net::BitReader reader(entry.payload.data(), entry.payload.size());
    out.health = static_cast<int32_t>(reader.readUInt32());
    out.max_health = static_cast<int32_t>(reader.readUInt32());
    out.alive = reader.readByte() != 0;
    if (reader.hasError() || reader.getBitPosition() != entry.payload.size() * 8u) {
        return false;
    }

    // Current FPS health is intentionally strict: no derived or legacy
    // variants enter the ECS bridge, and a dead pawn has zero health.
    return out.max_health >= 1 && out.max_health <= 10000 &&
           out.health >= 0 && out.health <= out.max_health &&
           out.alive == (out.health > 0);
}

inline bool extractHealth(const entt::registry& registry, entt::entity entity,
                          Net::ReplicatedExtEntry& out)
{
    const FpsHealthComponent* health = registry.try_get<FpsHealthComponent>(entity);
    if (health == nullptr) {
        return false;
    }

    out.component_id = FpsHealthComponentId;
    out.field_mask = FpsHealthFields;
    Net::BitWriter writer;
    writeInt32(writer, health->health);
    writeInt32(writer, health->max_health);
    writer.writeByte(health->alive ? 1u : 0u);
    out.payload.assign(writer.getData(), writer.getData() + writer.getByteSize());
    return true;
}

inline bool validateHealth(const Net::ReplicatedExtEntry& entry)
{
    FpsHealthComponent staged;
    return readHealthPayload(entry, staged);
}

inline void applyHealth(entt::registry& registry, entt::entity entity,
                        const Net::ReplicatedExtEntry& entry)
{
    FpsHealthComponent staged;
    // validateEcsEntries runs before every apply callback. Keep this guard so
    // an accidental direct call cannot write malformed wire data.
    if (!readHealthPayload(entry, staged)) {
        return;
    }
    registry.emplace_or_replace<FpsHealthComponent>(entity, staged);
}

inline bool registerReplicationComponents()
{
    return Net::ReplicationRegistry::get().registerComponent({
        FpsHealthComponentId,
        {{"health", 0}, {"max_health", 1}, {"alive", 2}},
        &extractHealth,
        &validateHealth,
        &applyHealth
    });
}

inline void unregisterReplicationComponents()
{
    (void)Net::ReplicationRegistry::get().unregisterComponent(FpsHealthComponentId);
}

} // namespace FPSShooter::Replication
