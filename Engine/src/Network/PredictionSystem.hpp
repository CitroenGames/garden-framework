#pragma once

#include "EngineExport.h"
#include "Network/PredictionTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

class world;
struct EntityRef;

namespace Net {

// Template code supplies the game-specific movement or physics operation.
// EngineCore owns history, correction ordering, ECS writes, and visual offset
// lifetime behind its private ECS bridge.
struct PredictionReplayAdapter
{
    std::function<bool(world&, EntityRef, const MovementState&)> restore_authoritative;
    std::function<MovementState(world&, EntityRef, const MovementInput&, const MovementState&)> simulate;
};

struct PredictionReconciliationConfig
{
    float minimum_correction_distance = 0.01f;
    float snap_distance = 10.0f;
};

enum class PredictionReconcileResult : uint8_t
{
    Rejected,
    IgnoredStale,
    Applied
};

struct PredictionDiagnostics
{
    std::size_t input_history_size = 0;
    bool has_authoritative_tick = false;
    uint64_t correction_count = 0;
    uint64_t replayed_input_count = 0;
    uint64_t stale_update_count = 0;
    uint64_t replay_failure_count = 0;
};

class ENGINE_API PredictionSystem
{
public:
    // Creates the neutral component set used by the optional prediction
    // feature. Existing game-created pawns can instead call recordInput once
    // their movement components are installed.
    static bool initializePawn(world& game_world, EntityRef pawn,
                               const MovementState& initial_state);

    // Associates prediction state with a fully constructed, network-owned
    // controller without changing its existing movement configuration.
    static bool attachPawn(world& game_world, EntityRef pawn);

    // Configures neutral controller movement for a pawn; invalid handles or
    // non-finite/negative values leave its configuration unchanged.
    static bool configureMovement(world& game_world, EntityRef pawn,
                                  float speed, float jump_velocity);

    static bool recordInput(world& game_world, EntityRef pawn,
                            uint32_t tick, const MovementInput& input,
                            const MovementState& predicted_state);

    static bool applyPredictedState(world& game_world, EntityRef pawn,
                                    const MovementState& state);
    static bool getMovementState(const world& game_world, EntityRef pawn,
                                 MovementState& out_state);
    static bool getPresentationState(const world& game_world, EntityRef pawn,
                                     MovementState& out_state);
    static bool getDiagnostics(const world& game_world, EntityRef pawn,
                               PredictionDiagnostics& out_diagnostics);
    static const InputRingBuffer<128>* getInputHistory(const world& game_world,
                                                        EntityRef pawn);

    static PredictionReconcileResult reconcile(world& game_world, EntityRef pawn,
                                                const MovementState& authoritative_state,
                                                uint32_t acknowledged_tick,
                                                const PredictionReplayAdapter& adapter,
                                                const PredictionReconciliationConfig& config = {});

    // Called once per render frame for the local pawn. It updates only the
    // transient render pose, never TransformComponent's authoritative pose.
    static void updatePresentation(world& game_world, EntityRef pawn,
                                   float delta_time);

    // Respawn/teleport boundaries intentionally discard replay and visual
    // correction state rather than blending across distinct pawn lives.
    static void reset(world& game_world, EntityRef pawn);
};

} // namespace Net
