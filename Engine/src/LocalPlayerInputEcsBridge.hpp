#pragma once

#include "LocalPlayerInputSystem.hpp"

// EngineCore-private bridge implementing the Garden local-input system API.
namespace LocalPlayerInputEcsBridge
{
void update(world& game_world, const InputActionSystem& input_actions);
void setOwner(world& game_world, EntityRef& current_owner, EntityRef new_owner,
              const LocalPlayerInputActionNames& actions);
}
