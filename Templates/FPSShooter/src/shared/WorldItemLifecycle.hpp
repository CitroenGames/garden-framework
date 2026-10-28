#pragma once

#include "Components/Components.hpp"
#include "Network/NetworkTypes.hpp"
#include "SharedComponents.hpp"
#include "FpsGameLogic.hpp"
#include "CompetitiveLogic.hpp"

#include <cmath>
#include <vector>

// Template-owned ECS lifecycle for weapon pickups. The server owns when this
// runs; these helpers only validate and mutate template weapon state.
namespace FpsLogic {

constexpr float kDroppedWeaponPickupRadius = 1.2f;
constexpr float kDroppedWeaponMinimumWorldY = kWorldItemMinimumWorldY;

inline bool isWeaponLifecyclePhaseOpen(Competitive::RoundPhase phase)
{
    using P = Competitive::RoundPhase;
    return phase == P::Warmup || phase == P::Freeze || phase == P::Live;
}

enum class DroppedWeaponPickupResult : uint8_t {
    PickedUp,
    MissingPlayer,
    MissingDroppedItem,
    InvalidDroppedItem,
    PlayerIneligible,
    OutOfRange,
    SlotOccupied,
    DropperMustLeavePickupRadius
};

struct DroppedWeaponPickup {
    entt::entity player = entt::null;
    entt::entity dropped_item = entt::null;
};

inline bool isValidDroppedWeapon(const DroppedWeaponComponent& item)
{
    const auto type = static_cast<uint8_t>(item.weapon_type);
    return type < static_cast<uint8_t>(WeaponType::COUNT) &&
           item.weapon_type != WeaponType::KNIFE &&
           item.ammo >= 0 && item.reserve >= 0;
}

inline bool isDroppedWeaponInWorldBounds(const TransformComponent& transform)
{
    const glm::vec3& position = transform.position;
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
           position.y >= kDroppedWeaponMinimumWorldY;
}

inline DroppedWeaponPickupResult tryPickupDroppedWeapon(
    entt::registry& registry,
    entt::entity player,
    entt::entity dropped_item,
    float pickup_radius = kDroppedWeaponPickupRadius)
{
    if (!registry.valid(player))
        return DroppedWeaponPickupResult::MissingPlayer;
    if (!registry.valid(dropped_item))
        return DroppedWeaponPickupResult::MissingDroppedItem;
    if (!registry.all_of<Net::NetworkedEntity, TransformComponent, FpsHealthComponent,
                         InventoryComponent, PlayerLoadoutComponent, WeaponComponent>(player))
        return DroppedWeaponPickupResult::PlayerIneligible;
    if (!registry.all_of<DroppedWeaponComponent, TransformComponent>(dropped_item))
        return DroppedWeaponPickupResult::InvalidDroppedItem;

    const auto& networked = registry.get<Net::NetworkedEntity>(player);
    const auto& health = registry.get<FpsHealthComponent>(player);
    const auto& dropped = registry.get<DroppedWeaponComponent>(dropped_item);
    const auto& player_transform = registry.get<TransformComponent>(player);
    const auto& dropped_transform = registry.get<TransformComponent>(dropped_item);
    if (!networked.is_player || !health.alive)
        return DroppedWeaponPickupResult::PlayerIneligible;
    if (!isValidDroppedWeapon(dropped) || !isDroppedWeaponInWorldBounds(dropped_transform) ||
        !isDroppedWeaponInWorldBounds(player_transform))
        return DroppedWeaponPickupResult::InvalidDroppedItem;
    if (!(pickup_radius >= 0.0f) || !std::isfinite(pickup_radius))
        return DroppedWeaponPickupResult::OutOfRange;
    const float pickup_distance = glm::distance(player_transform.position, dropped_transform.position);
    if (pickup_distance > pickup_radius) {
        // The only transition for manual-drop departure is an authoritative
        // owner position outside the same radius used for pickup.
        if (auto* exit = registry.try_get<DroppedWeaponDropperExitComponent>(dropped_item);
            exit && exit->must_leave_pickup_radius &&
            exit->blocked_client_id == networked.owner_client_id) {
            registry.remove<DroppedWeaponDropperExitComponent>(dropped_item);
        }
        return DroppedWeaponPickupResult::OutOfRange;
    }
    if (const auto* exit = registry.try_get<DroppedWeaponDropperExitComponent>(dropped_item);
        exit && exit->must_leave_pickup_radius &&
        exit->blocked_client_id == networked.owner_client_id) {
        return DroppedWeaponPickupResult::DropperMustLeavePickupRadius;
    }

    auto& inventory = registry.get<InventoryComponent>(player);
    auto& loadout = registry.get<PlayerLoadoutComponent>(player);
    auto& weapon = registry.get<WeaponComponent>(player);
    const DroppedItem item{dropped.weapon_type, dropped.ammo, dropped.reserve};
    return tryPickup(inventory, loadout, weapon, item)
        ? DroppedWeaponPickupResult::PickedUp
        : DroppedWeaponPickupResult::SlotOccupied;
}

// Processes each dropped entity once. A successful first candidate ends that
// item's search before destruction is deferred, giving one deterministic winner.
using DroppedWeaponRemovalHandler = void (*)(entt::registry&, entt::entity);

// The server can supply a removal hook to publish a world-item remove before
// the entity disappears. Standalone gameplay callers retain the direct-destroy
// default used by the focused lifecycle tests.
inline void removeDroppedWeapon(
    entt::registry& registry,
    entt::entity dropped_item,
    DroppedWeaponRemovalHandler before_destroy = nullptr)
{
    if (!registry.valid(dropped_item))
        return;
    if (before_destroy)
        before_destroy(registry, dropped_item);
    else
        registry.destroy(dropped_item);
}

inline std::vector<DroppedWeaponPickup> processDroppedWeaponPickups(
    entt::registry& registry,
    float pickup_radius = kDroppedWeaponPickupRadius,
    DroppedWeaponRemovalHandler before_destroy = nullptr)
{
    std::vector<DroppedWeaponPickup> pickups;
    auto dropped_view = registry.view<DroppedWeaponComponent, TransformComponent>();
    auto player_view = registry.view<Net::NetworkedEntity, TransformComponent, FpsHealthComponent,
                                     InventoryComponent, PlayerLoadoutComponent, WeaponComponent>();
    for (const entt::entity dropped_item : dropped_view) {
        if (!registry.valid(dropped_item))
            continue;
        for (const entt::entity player : player_view) {
            if (tryPickupDroppedWeapon(registry, player, dropped_item, pickup_radius) ==
                DroppedWeaponPickupResult::PickedUp) {
                pickups.push_back({player, dropped_item});
                break;
            }
        }
    }
    for (const DroppedWeaponPickup& pickup : pickups)
        removeDroppedWeapon(registry, pickup.dropped_item, before_destroy);
    return pickups;
}

inline void cleanupDroppedWeapons(
    entt::registry& registry,
    float delta_time,
    DroppedWeaponRemovalHandler before_destroy = nullptr)
{
    std::vector<entt::entity> expired;
    auto dropped_view = registry.view<DroppedWeaponComponent>();
    for (const entt::entity dropped_item : dropped_view) {
        auto& dropped = dropped_view.get<DroppedWeaponComponent>(dropped_item);
        const auto* transform = registry.try_get<TransformComponent>(dropped_item);
        if (delta_time > 0.0f && std::isfinite(delta_time))
            dropped.despawn_timer -= delta_time;
        if (!isValidDroppedWeapon(dropped) || !std::isfinite(dropped.despawn_timer) ||
            dropped.despawn_timer <= 0.0f || !transform || !isDroppedWeaponInWorldBounds(*transform))
            expired.push_back(dropped_item);
    }
    for (const entt::entity dropped_item : expired)
        removeDroppedWeapon(registry, dropped_item, before_destroy);
}

inline void clearDroppedWeapons(
    entt::registry& registry,
    DroppedWeaponRemovalHandler before_destroy = nullptr)
{
    std::vector<entt::entity> dropped_items;
    auto dropped_view = registry.view<DroppedWeaponComponent>();
    for (const entt::entity dropped_item : dropped_view)
        dropped_items.push_back(dropped_item);
    for (const entt::entity dropped_item : dropped_items)
        removeDroppedWeapon(registry, dropped_item, before_destroy);
}

} // namespace FpsLogic
