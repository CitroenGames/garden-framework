#pragma once

#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/ReplicationRegistry.hpp"
#include "Network/SpawnRecipeRegistry.hpp"
#include "SharedComponents.hpp"

#include <cstdint>
#include <vector>

namespace FPSShooter::Replication {

// Stable FPSShooter protocol id. This is template schema, never an EngineCore
// type, and all fields are current-version-only.
inline constexpr uint16_t FpsHealthComponentId = 0x1001;
inline constexpr uint16_t FpsHealthFields = 0x0007;
// The template reserves a stable non-player recipe id. EngineCore owns only
// the dispatch table; this recipe and FpsHealthComponent remain template code.
inline constexpr uint16_t FpsReplicatedActorArchetypeId = 0x2001;

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

inline bool validateReplicatedActorInitialState(
    const Net::SpawnEntityMessage&, const std::vector<Net::ReplicatedExtEntry>& entries)
{
    // This current template recipe has exactly one authored initial health
    // component. Schema drift cannot silently create a partially initialized
    // template entity.
    return entries.size() == 1 && entries[0].component_id == FpsHealthComponentId &&
           validateHealth(entries[0]);
}

inline bool constructReplicatedActor(entt::registry& registry, entt::entity entity,
                                     const Net::SpawnEntityMessage&)
{
    // The validated initial block immediately replaces this default. Adding
    // the concrete component here keeps the game-owned recipe explicit while
    // EngineCore only supplies identity and transform components.
    registry.emplace<FpsHealthComponent>(entity, FpsHealthComponent{});
    return true;
}

inline bool registerReplicationComponents()
{
    if (!Net::ReplicationRegistry::get().registerComponent({
            FpsHealthComponentId,
            {{"health", 0}, {"max_health", 1}, {"alive", 2}},
            &extractHealth,
            &validateHealth,
            &applyHealth
        })) {
        return false;
    }
    if (!Net::SpawnRecipeRegistry::get().registerRecipe({
            FpsReplicatedActorArchetypeId,
            &validateReplicatedActorInitialState,
            &constructReplicatedActor
        })) {
        (void)Net::ReplicationRegistry::get().unregisterComponent(FpsHealthComponentId);
        return false;
    }
    return true;
}

inline void unregisterReplicationComponents()
{
    (void)Net::SpawnRecipeRegistry::get().unregisterRecipe(FpsReplicatedActorArchetypeId);
    (void)Net::ReplicationRegistry::get().unregisterComponent(FpsHealthComponentId);
}

} // namespace FPSShooter::Replication
