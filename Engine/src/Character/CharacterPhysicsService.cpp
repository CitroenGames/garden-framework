#include "Character/CharacterPhysicsService.hpp"

#include "world.hpp"

// This implementation is the private ECS and physics bridge for the Garden API.
bool CharacterPhysicsService::createController(EntityRef entity)
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved))
        return false;

    m_world.physics_system->createCharacterController(m_world.registry, resolved);
    // A CharacterVirtual can be valid without an inner Jolt body.
    return m_world.physics_system->hasCharacterController(resolved);
}

bool CharacterPhysicsService::hasController(EntityRef entity) const
{
    const auto resolved = m_world.resolveEntity(entity);
    return resolved != entt::null &&
        m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved) &&
        m_world.physics_system->hasCharacterController(resolved);
}

bool CharacterPhysicsService::readControllerState(EntityRef entity, CharacterControllerState& out_state) const
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved) ||
        !m_world.physics_system->hasCharacterController(resolved))
        return false;
    out_state = m_world.physics_system->getCharacterControllerState(m_world.registry, resolved);
    return true;
}

bool CharacterPhysicsService::writeControllerState(EntityRef entity, const CharacterControllerState& state)
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved) ||
        !m_world.physics_system->hasCharacterController(resolved))
        return false;
    return m_world.physics_system->setCharacterControllerState(m_world.registry, resolved, state);
}

bool CharacterPhysicsService::simulateController(EntityRef entity, const CharacterMoveInput& input,
                                                 float delta_time, CharacterControllerState& out_state)
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved))
        return false;
    const CharacterControllerState state = m_world.physics_system->simulateCharacterController(
        m_world.registry, resolved, input, delta_time);
    // Simulation may recreate a controller when its shape changes.
    if (!m_world.physics_system->hasCharacterController(resolved))
        return false;
    out_state = state;
    return true;
}

bool CharacterPhysicsService::teleportController(EntityRef entity, const glm::vec3& position)
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved) ||
        !m_world.physics_system->hasCharacterController(resolved))
        return false;
    return m_world.physics_system->teleportCharacterController(m_world.registry, resolved, position);
}

bool CharacterPhysicsService::refreshController(EntityRef entity)
{
    const auto resolved = m_world.resolveEntity(entity);
    if (resolved == entt::null ||
        !m_world.registry.all_of<TransformComponent, CharacterControllerComponent>(resolved) ||
        !m_world.physics_system->hasCharacterController(resolved))
        return false;
    return m_world.physics_system->refreshCharacterController(m_world.registry, resolved);
}
