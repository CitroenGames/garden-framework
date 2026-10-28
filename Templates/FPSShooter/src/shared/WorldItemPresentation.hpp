#pragma once

#include "Components/Components.hpp"
#include "CombatProtocol.hpp"
#include "SharedComponents.hpp"

#include <cstdint>
#include <unordered_map>

// Renderer-free client ownership for template world-item presentation rows.
// The protocol key is intentionally local to this template and never enters
// EngineCore's snapshot-entity map.
namespace FpsLogic {

using WorldItemPresentationIndex = std::unordered_map<uint32_t, entt::entity>;

enum class WorldItemPresentationUpsertResult : uint8_t
{
    Rejected,
    Created,
    Updated
};

enum class WorldItemPresentationRemoveResult : uint8_t
{
    Rejected,
    Unknown,
    Removed
};

inline WorldItemPresentationUpsertResult upsertWorldItemPresentation(
    entt::registry& registry,
    WorldItemPresentationIndex& entities,
    const WorldItemUpsertMessage& message)
{
    if (!isValidWorldItemUpsert(message))
        return WorldItemPresentationUpsertResult::Rejected;

    auto entry = entities.find(message.world_item_id);
    if (entry != entities.end() && !registry.valid(entry->second)) {
        entities.erase(entry);
        entry = entities.end();
    }

    if (entry != entities.end() &&
        !registry.all_of<TransformComponent, DroppedWeaponPresentationComponent>(entry->second)) {
        registry.destroy(entry->second);
        entities.erase(entry);
        entry = entities.end();
    }

    WorldItemPresentationUpsertResult result = WorldItemPresentationUpsertResult::Updated;
    entt::entity entity = entt::null;
    if (entry == entities.end()) {
        entity = registry.create();
        registry.emplace<TransformComponent>(entity);
        registry.emplace<DroppedWeaponPresentationComponent>(
            entity, message.world_item_id, static_cast<WeaponType>(message.weapon_type));
        entities.emplace(message.world_item_id, entity);
        result = WorldItemPresentationUpsertResult::Created;
    } else {
        entity = entry->second;
    }

    TransformComponent& transform = registry.get<TransformComponent>(entity);
    DroppedWeaponPresentationComponent& presentation =
        registry.get<DroppedWeaponPresentationComponent>(entity);
    transform.position = message.position;
    transform.rotation.y = glm::degrees(message.yaw);
    presentation.world_item_id = message.world_item_id;
    presentation.weapon_type = static_cast<WeaponType>(message.weapon_type);
    return result;
}

inline WorldItemPresentationRemoveResult removeWorldItemPresentation(
    entt::registry& registry,
    WorldItemPresentationIndex& entities,
    const WorldItemRemoveMessage& message)
{
    if (!isValidWorldItemRemove(message))
        return WorldItemPresentationRemoveResult::Rejected;

    const auto entry = entities.find(message.world_item_id);
    if (entry == entities.end())
        return WorldItemPresentationRemoveResult::Unknown;

    if (registry.valid(entry->second))
        registry.destroy(entry->second);
    entities.erase(entry);
    return WorldItemPresentationRemoveResult::Removed;
}

inline void clearWorldItemPresentations(
    entt::registry& registry,
    WorldItemPresentationIndex& entities)
{
    for (const auto& [world_item_id, entity] : entities) {
        (void)world_item_id;
        if (registry.valid(entity))
            registry.destroy(entity);
    }
    entities.clear();
}

} // namespace FpsLogic
