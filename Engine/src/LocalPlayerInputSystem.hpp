#pragma once

#include "EngineExport.h"
#include "Components/LocalPlayerInputComponent.hpp"

struct EntityRef;
class world;

class InputActionSystem;

// Copies evaluated host action state into runtime ECS input intent.
ENGINE_API void updateLocalPlayerInputs(world& game_world,
                                        const InputActionSystem& input_actions);

// Moves the transient local-input component to the currently owned pawn.
// Any previous owner loses the component, so remote pawns never retain it.
ENGINE_API void setLocalPlayerInputOwner(world& game_world,
                                         EntityRef& current_owner,
                                         EntityRef new_owner,
                                         const LocalPlayerInputActionNames& actions);
