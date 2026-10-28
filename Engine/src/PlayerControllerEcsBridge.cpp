#include "PlayerController.hpp"

// Private EngineCore seam: adapt the Garden camera target to the physics
// query's current storage-entity exclusion until that query accepts EntityRef.
glm::vec3 PlayerController::resolveCameraSpringCollision(const glm::vec3& pivot,
    const glm::vec3& desired_position, const CameraSpringComponent& spring,
    EntityRef target_entity)
{
    if (!spring.collision_enabled)
        return desired_position;

    const glm::vec3 arm = desired_position - pivot;
    const float arm_length = glm::length(arm);
    if (arm_length <= 0.001f)
        return desired_position;

    const auto ignored = resolveEntityRef(game_world->registry, target_entity);
    const auto hit = game_world->raycastClosest(pivot, arm / arm_length, arm_length, ignored);
    if (!hit.hit)
        return desired_position;

    const float adjusted_distance = std::max(hit.distance - std::max(spring.collision_padding, 0.0f), 0.0f);
    return pivot + (arm / arm_length) * adjusted_distance;
}

void PlayerController::updatePlayerCollision()
{
    if (!game_world || !game_world->isEntityValid(player_entity))
        return;

    if (game_world->hasComponent<CharacterControllerComponent>(player_entity))
    {
        CharacterPhysicsService character_physics(*game_world);
        if (!character_physics.refreshController(player_entity))
            LOG_ENGINE_WARN("Failed to refresh local character controller");
        return;
    }

    const auto resolved = resolveEntityRef(game_world->registry, player_entity);
    if (resolved != entt::null)
        game_world->getPhysicsSystem().handlePlayerCollisions(game_world->registry, resolved);
}
