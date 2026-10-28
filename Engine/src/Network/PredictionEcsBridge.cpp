#include "Network/PredictionEcsBridge.hpp"

#include "Components/ClientPredictionComponent.hpp"
#include "Components/Components.hpp"
#include "Network/NetworkHierarchyValidation.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>

namespace Net::detail {
namespace {

entt::entity resolve(world& game_world, EntityRef pawn)
{
    return resolveEntityRef(game_world.registry, pawn);
}

entt::entity resolve(const world& game_world, EntityRef pawn)
{
    return resolveEntityRef(game_world.registry, pawn);
}

bool isFinite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isFinite(const MovementState& state)
{
    return isFinite(state.position) && isFinite(state.velocity) &&
           isFinite(state.ground_normal) && isFinite(state.ground_velocity);
}

bool hasMovementComponents(const entt::registry& registry, entt::entity pawn)
{
    return pawn != entt::null && registry.valid(pawn) &&
        registry.all_of<TransformComponent, RigidBodyComponent, CharacterControllerComponent>(pawn);
}

void writeMovementState(entt::registry& registry, entt::entity pawn,
                        const MovementState& state)
{
    auto& transform = registry.get<TransformComponent>(pawn);
    auto& rigid_body = registry.get<RigidBodyComponent>(pawn);
    auto& controller = registry.get<CharacterControllerComponent>(pawn);
    transform.position = state.position;
    rigid_body.velocity = state.velocity;
    controller.grounded = state.grounded;
    controller.ground_normal = state.ground_normal;
}

MovementState readMovementState(const entt::registry& registry, entt::entity pawn)
{
    const auto& transform = registry.get<TransformComponent>(pawn);
    const auto& rigid_body = registry.get<RigidBodyComponent>(pawn);
    const auto& controller = registry.get<CharacterControllerComponent>(pawn);
    MovementState state;
    state.position = transform.position;
    state.velocity = rigid_body.velocity;
    state.grounded = controller.grounded;
    state.ground_normal = controller.ground_normal;
    return state;
}

void writePresentation(entt::registry& registry, entt::entity pawn,
                       const ClientPredictionComponent& prediction)
{
    const auto& transform = registry.get<TransformComponent>(pawn);
    PresentationTransformComponent presentation;
    presentation.position = prediction.smoothing.getDisplayPosition(transform.position);
    presentation.rotation = transform.rotation;
    registry.emplace_or_replace<PresentationTransformComponent>(pawn, presentation);
}

} // namespace

bool initializePredictionPawn(world& game_world, EntityRef pawn,
                              const MovementState& initial_state)
{
    entt::entity entity = resolve(game_world, pawn);
    if (entity == entt::null || !isFinite(initial_state))
        return false;

    auto& registry = game_world.registry;
    registry.emplace_or_replace<TransformComponent>(entity, TransformComponent{}).position = initial_state.position;
    registry.emplace_or_replace<RigidBodyComponent>(entity, RigidBodyComponent{}).velocity = initial_state.velocity;
    auto& controller = registry.emplace_or_replace<CharacterControllerComponent>(entity);
    controller.grounded = initial_state.grounded;
    controller.ground_normal = initial_state.ground_normal;
    registry.emplace_or_replace<ClientPredictionComponent>(entity);
    PresentationTransformComponent presentation;
    presentation.position = initial_state.position;
    registry.emplace_or_replace<PresentationTransformComponent>(entity, presentation);
    return true;
}

bool attachPredictionPawn(world& game_world, EntityRef pawn)
{
    const entt::entity entity = resolve(game_world, pawn);
    auto& registry = game_world.registry;
    if (!hasMovementComponents(registry, entity))
        return false;

    registry.get_or_emplace<ClientPredictionComponent>(entity);
    const auto& transform = registry.get<TransformComponent>(entity);
    PresentationTransformComponent presentation;
    presentation.position = transform.position;
    presentation.rotation = transform.rotation;
    registry.emplace_or_replace<PresentationTransformComponent>(entity, presentation);
    return true;
}

bool configurePredictionMovement(world& game_world, EntityRef pawn,
                                 float speed, float jump_velocity)
{
    if (!std::isfinite(speed) || speed < 0.0f ||
        !std::isfinite(jump_velocity) || jump_velocity < 0.0f)
        return false;
    const entt::entity entity = resolve(game_world, pawn);
    auto& registry = game_world.registry;
    if (entity == entt::null || !registry.all_of<CharacterControllerComponent>(entity))
        return false;
    auto& controller = registry.get<CharacterControllerComponent>(entity);
    controller.move_speed = speed;
    controller.jump_velocity = jump_velocity;
    if (auto* player = registry.try_get<PlayerComponent>(entity)) {
        player->speed = speed;
        player->jump_force = jump_velocity;
    }
    return true;
}

bool applyPredictedMovementState(world& game_world, EntityRef pawn,
                                 const MovementState& state)
{
    entt::entity entity = resolve(game_world, pawn);
    if (!isFinite(state) || !hasMovementComponents(game_world.registry, entity))
        return false;
    writeMovementState(game_world.registry, entity, state);
    return true;
}

bool readPredictedMovementState(const world& game_world, EntityRef pawn,
                                MovementState& out_state)
{
    const entt::entity entity = resolve(game_world, pawn);
    if (!hasMovementComponents(game_world.registry, entity))
        return false;
    out_state = readMovementState(game_world.registry, entity);
    return true;
}

bool readPredictionDiagnostics(const world& game_world, EntityRef pawn,
                               PredictionDiagnostics& out_diagnostics)
{
    const entt::entity entity = resolve(game_world, pawn);
    const auto* prediction = entity == entt::null ? nullptr :
        game_world.registry.try_get<ClientPredictionComponent>(entity);
    if (!prediction)
        return false;
    out_diagnostics.input_history_size = prediction->input_history.size();
    out_diagnostics.has_authoritative_tick = prediction->has_authoritative_tick;
    out_diagnostics.correction_count = prediction->correction_count;
    out_diagnostics.replayed_input_count = prediction->replayed_input_count;
    out_diagnostics.stale_update_count = prediction->stale_update_count;
    out_diagnostics.replay_failure_count = prediction->replay_failure_count;
    return true;
}

bool readPredictionPresentationState(const world& game_world, EntityRef pawn,
                                     MovementState& out_state)
{
    const entt::entity entity = resolve(game_world, pawn);
    const auto* transform = entity == entt::null ? nullptr :
        game_world.registry.try_get<TransformComponent>(entity);
    const auto* presentation = entity == entt::null ? nullptr :
        game_world.registry.try_get<PresentationTransformComponent>(entity);
    if (!transform || !presentation)
        return false;
    out_state.position = presentation->position;
    out_state.velocity = game_world.registry.all_of<RigidBodyComponent>(entity)
        ? game_world.registry.get<RigidBodyComponent>(entity).velocity : glm::vec3(0.0f);
    return true;
}

bool recordPredictionInput(world& game_world, EntityRef pawn, uint32_t tick,
                           const MovementInput& input,
                           const MovementState& predicted_state)
{
    const entt::entity entity = resolve(game_world, pawn);
    auto& registry = game_world.registry;
    if (!hasMovementComponents(registry, entity) || !isNetworkHierarchyRoot(registry, entity) ||
        !isFinite(predicted_state)) {
        return false;
    }

    auto& prediction = registry.get_or_emplace<ClientPredictionComponent>(entity);
    PredictionEntry entry;
    entry.tick = tick;
    entry.input = input;
    entry.predicted_state = predicted_state;
    entry.valid = true;
    prediction.input_history.push(entry);
    return true;
}

const InputRingBuffer<128>* getPredictionInputHistory(const world& game_world,
                                                       EntityRef pawn)
{
    const entt::entity entity = resolve(game_world, pawn);
    if (entity == entt::null)
        return nullptr;
    const auto* prediction = game_world.registry.try_get<ClientPredictionComponent>(entity);
    return prediction ? &prediction->input_history : nullptr;
}

PredictionReconcileResult reconcilePrediction(
    world& game_world, EntityRef pawn, const MovementState& authoritative_state,
    uint32_t acknowledged_tick, const PredictionReplayAdapter& adapter,
    const PredictionReconciliationConfig& config)
{
    entt::registry& registry = game_world.registry;
    const entt::entity entity = resolve(game_world, pawn);
    if (!hasMovementComponents(registry, entity) || !isNetworkHierarchyRoot(registry, entity) ||
        !isFinite(authoritative_state) || !std::isfinite(config.minimum_correction_distance) ||
        !std::isfinite(config.snap_distance) || config.minimum_correction_distance < 0.0f ||
        config.snap_distance < config.minimum_correction_distance || !adapter.simulate) {
        return PredictionReconcileResult::Rejected;
    }

    auto& prediction = registry.get_or_emplace<ClientPredictionComponent>(entity);
    if (prediction.has_authoritative_tick &&
        !isTickNewer(acknowledged_tick, prediction.last_authoritative_tick)) {
        ++prediction.stale_update_count;
        return PredictionReconcileResult::IgnoredStale;
    }

    const auto& transform = registry.get<TransformComponent>(entity);
    const glm::vec3 old_display_position = prediction.smoothing.getDisplayPosition(transform.position);
    // The snapshot describes the acknowledged command, not the latest local
    // pose. Compare like ticks so ordinary movement does not cause a rewind.
    // If that command has fallen out of history, retain the conservative
    // current-pose comparison and replay the available commands.
    const PredictionEntry* acknowledged = prediction.input_history.get(acknowledged_tick);
    const glm::vec3& comparison_position = acknowledged
        ? acknowledged->predicted_state.position : transform.position;
    const float position_error = glm::distance(authoritative_state.position, comparison_position);
    prediction.last_error_meters = position_error;
    prediction.max_error_meters = std::max(prediction.max_error_meters, position_error);
    prediction.last_authoritative_tick = acknowledged_tick;
    prediction.has_authoritative_tick = true;

    if (position_error <= config.minimum_correction_distance) {
        prediction.input_history.discardUpTo(acknowledged_tick);
        return PredictionReconcileResult::Applied;
    }

    if (adapter.restore_authoritative &&
        !adapter.restore_authoritative(game_world, pawn, authoritative_state)) {
        ++prediction.replay_failure_count;
        return PredictionReconcileResult::Rejected;
    }

    MovementState replay_state = authoritative_state;
    bool replay_failed = false;
    prediction.input_history.forEachFrom(acknowledged_tick,
        [&](const PredictionEntry& entry) {
            if (replay_failed)
                return;
            const MovementState next = adapter.simulate(game_world, pawn, entry.input, replay_state);
            if (!isFinite(next)) {
                replay_failed = true;
                return;
            }
            replay_state = next;
            ++prediction.replayed_input_count;
        });
    if (replay_failed) {
        ++prediction.replay_failure_count;
        return PredictionReconcileResult::Rejected;
    }

    writeMovementState(registry, entity, replay_state);
    prediction.input_history.discardUpTo(acknowledged_tick);
    ++prediction.correction_count;
    if (position_error >= config.snap_distance) {
        prediction.smoothing.visual_offset = glm::vec3(0.0f);
    } else {
        prediction.smoothing.onCorrection(old_display_position, replay_state.position);
    }
    writePresentation(registry, entity, prediction);
    return PredictionReconcileResult::Applied;
}

void updatePredictionPresentation(world& game_world, EntityRef pawn, float delta_time)
{
    const entt::entity entity = resolve(game_world, pawn);
    auto& registry = game_world.registry;
    if (entity == entt::null || !registry.valid(entity) ||
        !registry.all_of<ClientPredictionComponent, TransformComponent>(entity)) {
        return;
    }
    auto& prediction = registry.get<ClientPredictionComponent>(entity);
    prediction.smoothing.update(delta_time);
    writePresentation(registry, entity, prediction);
}

void resetPrediction(world& game_world, EntityRef pawn)
{
    const entt::entity entity = resolve(game_world, pawn);
    auto& registry = game_world.registry;
    if (entity == entt::null || !registry.valid(entity))
        return;

    if (auto* prediction = registry.try_get<ClientPredictionComponent>(entity)) {
        prediction->input_history.clear();
        prediction->smoothing.visual_offset = glm::vec3(0.0f);
        prediction->has_authoritative_tick = false;
        prediction->last_authoritative_tick = 0;
    }
    if (registry.all_of<TransformComponent>(entity)) {
        const auto& transform = registry.get<TransformComponent>(entity);
        PresentationTransformComponent presentation;
        presentation.position = transform.position;
        presentation.rotation = transform.rotation;
        registry.emplace_or_replace<PresentationTransformComponent>(entity, presentation);
    }
}

} // namespace Net::detail
