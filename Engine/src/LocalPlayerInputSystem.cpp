#include "LocalPlayerInputSystem.hpp"
#include "LocalPlayerInputEcsBridge.hpp"
#include "Components/IdentityComponent.hpp"

void updateLocalPlayerInputs(world& game_world, const InputActionSystem& input_actions)
{
    LocalPlayerInputEcsBridge::update(game_world, input_actions);
}

void setLocalPlayerInputOwner(world& game_world, EntityRef& current_owner,
                              EntityRef new_owner, const LocalPlayerInputActionNames& actions)
{
    LocalPlayerInputEcsBridge::setOwner(game_world, current_owner, new_owner, actions);
}
