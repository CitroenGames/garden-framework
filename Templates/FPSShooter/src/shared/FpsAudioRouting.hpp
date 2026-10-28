#pragma once

#include "Audio/AudioEventRouter.hpp"
#include "CombatProtocol.hpp"

#include <cstdint>
#include <string_view>

// These mappings are template data. EngineCore receives only opaque event
// names, source IDs and ordering information through AudioEventRouter.
namespace FpsAudio
{
inline constexpr std::string_view kWeaponFireEvent = "weapon.fire";
inline constexpr std::string_view kPlayerHurtEvent = "player.hurt";
inline constexpr std::string_view kPlayerDeathEvent = "player.death";

// One actor can own independent authoritative streams (damage and death use
// different existing sequence domains), so the opaque engine source key also
// carries a template-local stream kind.
inline constexpr uint64_t presentationSource(uint16_t client_id, uint8_t stream_kind)
{
    return (static_cast<uint64_t>(client_id) << 8) | stream_kind;
}

inline AudioPresentationEvent makeFireEvent(
    const ShootResultMessage& message, uint16_t local_client_id)
{
    return {
        presentationSource(message.shooter_client_id, 1),
        message.presentation_sequence,
        kWeaponFireEvent,
        message.ray_origin,
        message.shooter_client_id == local_client_id
            ? AudioPresentationRoute::Local2D
            : AudioPresentationRoute::RemoteSpatial,
    };
}

inline AudioPresentationEvent makeHurtEvent(
    const DamageEventMessage& message, uint16_t local_client_id)
{
    return {
        presentationSource(message.victim_client_id, 2),
        message.presentation_sequence,
        kPlayerHurtEvent,
        message.hit_position,
        message.victim_client_id == local_client_id
            ? AudioPresentationRoute::Local2D
            : AudioPresentationRoute::RemoteSpatial,
    };
}

inline AudioPresentationEvent makeDeathEvent(
    const CombatEventMessage& message, uint16_t local_client_id, const glm::vec3& position)
{
    return {
        presentationSource(message.victim_client_id, 3),
        message.death_sequence,
        kPlayerDeathEvent,
        position,
        message.victim_client_id == local_client_id
            ? AudioPresentationRoute::Local2D
            : AudioPresentationRoute::RemoteSpatial,
    };
}
} // namespace FpsAudio
