#include "Network/PredictionSystem.hpp"

#include "Network/PredictionEcsBridge.hpp"

namespace Net {

bool PredictionSystem::initializePawn(world& game_world, EntityRef pawn,
                                      const MovementState& initial_state)
{
    return detail::initializePredictionPawn(game_world, pawn, initial_state);
}

bool PredictionSystem::attachPawn(world& game_world, EntityRef pawn)
{
    return detail::attachPredictionPawn(game_world, pawn);
}

bool PredictionSystem::configureMovement(world& game_world, EntityRef pawn,
                                         float speed, float jump_velocity)
{
    return detail::configurePredictionMovement(game_world, pawn, speed, jump_velocity);
}

bool PredictionSystem::recordInput(world& game_world, EntityRef pawn,
                                   uint32_t tick, const MovementInput& input,
                                   const MovementState& predicted_state)
{
    return detail::recordPredictionInput(game_world, pawn, tick, input, predicted_state);
}

bool PredictionSystem::applyPredictedState(world& game_world, EntityRef pawn,
                                           const MovementState& state)
{
    return detail::applyPredictedMovementState(game_world, pawn, state);
}

bool PredictionSystem::getMovementState(const world& game_world, EntityRef pawn,
                                        MovementState& out_state)
{
    return detail::readPredictedMovementState(game_world, pawn, out_state);
}

bool PredictionSystem::getPresentationState(const world& game_world, EntityRef pawn,
                                            MovementState& out_state)
{
    return detail::readPredictionPresentationState(game_world, pawn, out_state);
}

bool PredictionSystem::getDiagnostics(const world& game_world, EntityRef pawn,
                                      PredictionDiagnostics& out_diagnostics)
{
    return detail::readPredictionDiagnostics(game_world, pawn, out_diagnostics);
}

const InputRingBuffer<128>* PredictionSystem::getInputHistory(
    const world& game_world, EntityRef pawn)
{
    return detail::getPredictionInputHistory(game_world, pawn);
}

PredictionReconcileResult PredictionSystem::reconcile(
    world& game_world, EntityRef pawn, const MovementState& authoritative_state,
    uint32_t acknowledged_tick, const PredictionReplayAdapter& adapter,
    const PredictionReconciliationConfig& config)
{
    return detail::reconcilePrediction(game_world, pawn, authoritative_state,
                                       acknowledged_tick, adapter, config);
}

void PredictionSystem::updatePresentation(world& game_world, EntityRef pawn,
                                          float delta_time)
{
    detail::updatePredictionPresentation(game_world, pawn, delta_time);
}

void PredictionSystem::reset(world& game_world, EntityRef pawn)
{
    detail::resetPrediction(game_world, pawn);
}

} // namespace Net
