#pragma once

#include "EngineExport.h"
#include "Character/CharacterController.hpp"
#include "Components/IdentityComponent.hpp"

class world;

// A non-owning service bound to one world. The world must outlive the service.
// hasController reports whether the entity has the required components and a
// live runtime controller. simulateController can create the runtime controller
// lazily. State, teleport and refresh operations require an existing runtime
// controller. Output state changes only on success.
class ENGINE_API CharacterPhysicsService
{
public:
    explicit CharacterPhysicsService(world& world_context) : m_world(world_context) {}

    bool createController(EntityRef entity);
    bool hasController(EntityRef entity) const;
    bool readControllerState(EntityRef entity, CharacterControllerState& out_state) const;
    bool writeControllerState(EntityRef entity, const CharacterControllerState& state);
    bool simulateController(EntityRef entity, const CharacterMoveInput& input, float delta_time,
                            CharacterControllerState& out_state);
    bool teleportController(EntityRef entity, const glm::vec3& position);
    bool refreshController(EntityRef entity);

private:
    world& m_world;
};
