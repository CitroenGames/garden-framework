#pragma once

#include "Components/IdentityComponent.hpp"
#include "Network/PredictionSystem.hpp"

class world;

namespace Net::detail {

// Private EnTT bridge for the public prediction API. Gameplay and engine
// features use world plus EntityRef; storage access stays in this translation
// unit so EnTT lifetime and layout do not escape the ECS runtime.
bool initializePredictionPawn(world& game_world, EntityRef pawn,
                              const MovementState& initial_state);
bool attachPredictionPawn(world& game_world, EntityRef pawn);
bool configurePredictionMovement(world& game_world, EntityRef pawn,
                                 float speed, float jump_velocity);
bool applyPredictedMovementState(world& game_world, EntityRef pawn,
                                 const MovementState& state);
bool readPredictedMovementState(const world& game_world, EntityRef pawn,
                                MovementState& out_state);
bool readPredictionDiagnostics(const world& game_world, EntityRef pawn,
                               PredictionDiagnostics& out_diagnostics);
bool readPredictionPresentationState(const world& game_world, EntityRef pawn,
                                     MovementState& out_state);
bool recordPredictionInput(world& game_world, EntityRef pawn, uint32_t tick,
                           const MovementInput& input,
                           const MovementState& predicted_state);
const InputRingBuffer<128>* getPredictionInputHistory(const world& game_world,
                                                       EntityRef pawn);
PredictionReconcileResult reconcilePrediction(world& game_world, EntityRef pawn,
                                               const MovementState& authoritative_state,
                                               uint32_t acknowledged_tick,
                                               const PredictionReplayAdapter& adapter,
                                               const PredictionReconciliationConfig& config);
void updatePredictionPresentation(world& game_world, EntityRef pawn, float delta_time);
void resetPrediction(world& game_world, EntityRef pawn);

} // namespace Net::detail
