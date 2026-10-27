#pragma once

#include "EngineExport.h"
#include "Components/LocalPlayerInputComponent.hpp"

#include <entt/entt.hpp>

class InputActionSystem;

// Copies evaluated host action state into runtime ECS input intent.
ENGINE_API void updateLocalPlayerInputs(entt::registry& registry,
                                        const InputActionSystem& input_actions);

// Moves the transient local-input component to the currently owned pawn.
// Any previous owner loses the component, so remote pawns never retain it.
ENGINE_API void setLocalPlayerInputOwner(entt::registry& registry,
                                         entt::entity& current_owner,
                                         entt::entity new_owner,
                                         const LocalPlayerInputActionNames& actions);
