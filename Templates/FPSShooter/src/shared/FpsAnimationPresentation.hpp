#pragma once

#include "SharedComponents.hpp"

#include "Components/Components.hpp"
#include "Gameplay/GameplayComponents.hpp"

#include <cmath>
#include <cstdint>

namespace FPSShooter
{
    // Runtime-only, template-owned inputs for future animation content. These
    // values select no clips and do not participate in movement or networking.
    struct FpsAnimationParametersComponent
    {
        float horizontal_speed = 0.0f;
        uint8_t team_id = 0;
        int32_t active_weapon_slot = -1;
        bool has_active_weapon = false;
        bool idle = false;
        bool moving = false;
        bool jumping = false;
        bool reloading = false;
        bool firing = false;
        bool dead = false;
    };

    inline FpsAnimationParametersComponent deriveAnimationParameters(
        const FpsHealthComponent& health,
        const CharacterControllerComponent& controller,
        const RigidBodyComponent& rigidbody,
        const TeamComponent& team,
        const PlayerLoadoutComponent& loadout,
        const WeaponComponent* weapon)
    {
        FpsAnimationParametersComponent parameters;
        parameters.team_id = team.team_id;

        const glm::vec2 horizontal_velocity(rigidbody.velocity.x, rigidbody.velocity.z);
        const float speed_squared = glm::dot(horizontal_velocity, horizontal_velocity);
        if (std::isfinite(speed_squared) && speed_squared > 0.0f)
            parameters.horizontal_speed = std::sqrt(speed_squared);

        const int32_t active_slot = loadout.active_slot;
        if (active_slot >= 0 && active_slot < static_cast<int32_t>(WeaponSlot::COUNT) &&
            loadout.slots[active_slot].occupied)
        {
            parameters.active_weapon_slot = active_slot;
            parameters.has_active_weapon = true;
        }

        const bool alive = health.alive && health.health > 0;
        parameters.dead = !alive;
        if (!alive)
            return parameters;

        constexpr float movement_epsilon = 0.01f;
        parameters.moving = parameters.horizontal_speed > movement_epsilon;
        parameters.idle = controller.grounded && !parameters.moving;
        // The controller exposes grounded state, but no distinct airborne
        // subtype. This is therefore the available jump/air presentation bit.
        parameters.jumping = !controller.grounded;

        if (weapon != nullptr)
        {
            parameters.reloading = weapon->reloading;
            // A positive cooldown is produced only by a successful weapon
            // fire. It is the existing bounded presentation window; no new
            // fire event or clip contract is introduced here.
            parameters.firing = !parameters.reloading &&
                std::isfinite(weapon->fire_cooldown) && weapon->fire_cooldown > 0.0f;
        }

        return parameters;
    }

    inline void updateAnimationParameters(entt::registry& registry)
    {
        auto pawns = registry.view<FpsHealthComponent, CharacterControllerComponent,
            RigidBodyComponent, TeamComponent, PlayerLoadoutComponent>();
        for (const entt::entity pawn : pawns)
        {
            const auto* weapon = registry.try_get<WeaponComponent>(pawn);
            registry.emplace_or_replace<FpsAnimationParametersComponent>(pawn,
                deriveAnimationParameters(
                    pawns.get<FpsHealthComponent>(pawn),
                    pawns.get<CharacterControllerComponent>(pawn),
                    pawns.get<RigidBodyComponent>(pawn),
                    pawns.get<TeamComponent>(pawn),
                    pawns.get<PlayerLoadoutComponent>(pawn),
                    weapon));
        }

        // Representations use the established stable tracked-player reference.
        // Do not create ownership layers or mutate mesh visibility here.
        auto representations = registry.view<PlayerRepresentationComponent>();
        for (const entt::entity representation : representations)
        {
            const entt::entity tracked = resolveEntityRef(
                registry, representations.get<PlayerRepresentationComponent>(representation).tracked_player);
            const auto* parameters = (tracked != entt::null && registry.valid(tracked))
                ? registry.try_get<FpsAnimationParametersComponent>(tracked)
                : nullptr;
            if (parameters != nullptr)
                registry.emplace_or_replace<FpsAnimationParametersComponent>(representation, *parameters);
            else
                registry.remove<FpsAnimationParametersComponent>(representation);
        }
    }
}
