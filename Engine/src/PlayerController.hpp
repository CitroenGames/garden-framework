#pragma once

#include "EngineExport.h"
#include "Character/CharacterPhysicsService.hpp"
#include "Components/Components.hpp"
#include "Components/LocalPlayerInputComponent.hpp"
#include "Components/camera.hpp"
#include "InputManager.hpp"
#include "MovementTrace.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace GameFramework { class GameModeBase; }

enum class PossessedEntityType
{
    Player,
    Freecam
};

class ENGINE_API PlayerController
{
private:
    friend class GameFramework::GameModeBase;
    EntityRef player_entity;
    EntityRef freecam_entity;
    std::shared_ptr<InputManager> input_manager;
    PossessedEntityType currently_possessed;
    bool freecam_mode_enabled;
    bool movement_enabled = true; // When false, updatePlayer() only does camera follow (networked mode)
    world* game_world;
    glm::vec3 camera_spring_velocity = glm::vec3(0.0f);
    EntityRef camera_spring_entity{};
    bool camera_spring_initialized = false;
    float character_delta_accumulator = 0.0f;
    bool local_player_look_applied = false;

public:
    PlayerController(std::shared_ptr<InputManager> input_mgr, world* w)
        : player_entity{},
          freecam_entity{},
          input_manager(input_mgr),
          currently_possessed(PossessedEntityType::Player),
          freecam_mode_enabled(false),
          game_world(w)
    {
        setup_input_bindings();
    }

    void setup_input_bindings()
    {
        if (!input_manager) return;

        input_manager->bind_action("ToggleFreecam", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                toggleFreecamMode();
            }
        });

        input_manager->bind_action("Quit", [this](InputActionState state) {
            if (state == InputActionState::Pressed)
            {
                exit(0);
            }
        });
    }

    // Refresh collision and ground state for the currently assigned player.
    void updatePlayerCollision();

    void setPossessedPlayer(EntityRef player)
    {
        player_entity = player;
        resetCameraSpring();
    }

    void setPossessedFreecam(EntityRef freecam)
    {
        freecam_entity = freecam;
    }

    void toggleFreecamMode()
    {
        if (!game_world || !game_world->isEntityValid(player_entity) || !game_world->isEntityValid(freecam_entity) ||
            !game_world->hasComponent<TransformComponent>(player_entity) ||
            !game_world->hasComponent<TransformComponent>(freecam_entity))
            return;

        freecam_mode_enabled = !freecam_mode_enabled;
        resetCameraSpring();

        if (freecam_mode_enabled)
        {
            currently_possessed = PossessedEntityType::Freecam;

            auto& p_trans = *game_world->tryGetComponent<TransformComponent>(player_entity);
            auto& f_trans = *game_world->tryGetComponent<TransformComponent>(freecam_entity);
            f_trans.position = p_trans.position;

            printf("Switched to freecam mode\n");
        }
        else
        {
            currently_possessed = PossessedEntityType::Player;
            printf("Switched to player mode\n");
        }
    }

    void handleMouseMotion(float yrel, float xrel)
    {
        if (!game_world) return;

        camera& active_cam = game_world->world_camera;
        float sensitivity = 1.0f;

        if (currently_possessed == PossessedEntityType::Player && game_world->hasComponent<PlayerComponent>(player_entity))
        {
            auto& pc = *game_world->tryGetComponent<PlayerComponent>(player_entity);
            sensitivity = pc.mouse_sensitivity;
        }
        else if (currently_possessed == PossessedEntityType::Freecam && game_world->hasComponent<FreecamComponent>(freecam_entity))
        {
            auto& fc = *game_world->tryGetComponent<FreecamComponent>(freecam_entity);
            sensitivity = fc.mouse_sensitivity;
        }

        float effective_sensitivity_x = input_manager ? input_manager->Sensitivity_X * sensitivity : sensitivity;
        float effective_sensitivity_y = input_manager ? input_manager->Sensitivity_Y * sensitivity : sensitivity;

        active_cam.rotation.x += yrel / 1000.0f * effective_sensitivity_y;
        active_cam.rotation.y += -xrel / 1000.0f * effective_sensitivity_x;

        if (active_cam.rotation.x > 1.5f) active_cam.rotation.x = 1.5f;
        if (active_cam.rotation.x < -1.5f) active_cam.rotation.x = -1.5f;
    }

    void update(float delta)
    {
        static std::atomic<uint64_t> controller_frame_counter{0};
        const uint64_t trace_frame_id = MovementTrace::enabled() ? controller_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        MovementTrace::FrameSummary trace_frame;
        if (MovementTrace::enabled())
            MovementTrace::emit("offline_frame_begin", "role=client frame_scope=controller_update frame_id={} sdl_frame_ns={} sdl_clock=SDL_GetTicksNS", trace_frame_id, SDL_GetTicksNS());
        trace_frame = {};
        trace_frame.reason = "world_missing";
        if (!game_world)
        {
            MovementTrace::emit("offline_frame", "role=client frame_scope=controller_update frame_id={} steps=0 category=zero reason=world_missing", trace_frame_id);
            return;
        }
        trace_frame.reason = "possession_unavailable";

        if (currently_possessed == PossessedEntityType::Player)
        {
            updatePlayer(delta, trace_frame_id, trace_frame);
        }
        else if (currently_possessed == PossessedEntityType::Freecam)
        {
            updateFreecam(delta, trace_frame_id, trace_frame);
        }

        MovementTrace::emit("offline_frame", "role=client frame_scope=controller_update frame_id={} steps={} category={} reason={}",
            trace_frame_id, trace_frame.steps, trace_frame.category(), trace_frame.reason);
        local_player_look_applied = false;
    }

    // Hosts that sample camera angles before the controller's movement update
    // can apply projected local-player look first. update() consumes it only
    // once; standalone hosts can rely on update() to apply it automatically.
    void updatePlayerLook(float delta)
    {
        if (!game_world || currently_possessed != PossessedEntityType::Player ||
            !game_world->isEntityValid(player_entity))
            return;

        const auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity);
        if (!local_input)
            return;

        applyGamepadLook(*local_input, delta);
        local_player_look_applied = true;
    }

private:
    void applyGamepadLook(const LocalPlayerInputComponent& local_input, float delta)
    {
        if (!game_world || !game_world->isEntityValid(player_entity) ||
            !std::isfinite(delta) || delta <= 0.0f)
            return;

        float sensitivity = 1.0f;
        if (game_world->hasComponent<PlayerComponent>(player_entity))
        {
            const PlayerComponent& player = *game_world->tryGetComponent<PlayerComponent>(player_entity);
            if (!player.input_enabled)
                return;
            sensitivity = player.mouse_sensitivity;
        }
        if (game_world->hasComponent<CharacterControllerComponent>(player_entity) &&
            !game_world->tryGetComponent<CharacterControllerComponent>(player_entity)->input_enabled)
            return;

        if (!local_input.input_enabled)
            return;

        const float sensitivity_x = input_manager ? input_manager->Sensitivity_X * sensitivity : sensitivity;
        const float sensitivity_y = input_manager ? input_manager->Sensitivity_Y * sensitivity : sensitivity;
        camera& active_cam = game_world->world_camera;
        active_cam.rotation.y -= local_input.look.x * delta * sensitivity_x;
        active_cam.rotation.x += local_input.look.y * delta * sensitivity_y;
        active_cam.rotation.x = std::clamp(active_cam.rotation.x, -1.5f, 1.5f);
    }

    void resetCameraSpring()
    {
        camera_spring_velocity = glm::vec3(0.0f);
        camera_spring_entity = EntityRef{};
        camera_spring_initialized = false;
    }

    glm::vec3 resolveCameraSpringCollision(const glm::vec3& pivot,
                                           const glm::vec3& desired_position,
                                           const CameraSpringComponent& spring,
                                           EntityRef target_entity);

    glm::vec3 stepCameraSpring(const glm::vec3& current_position,
                               const glm::vec3& target_position,
                               float delta,
                               const CameraSpringComponent& spring)
    {
        if (!spring.position_lag_enabled || delta <= 0.0f)
        {
            camera_spring_velocity = glm::vec3(0.0f);
            return target_position;
        }

        const float omega = std::max(spring.spring_frequency, 0.001f);
        const float frame_delta = std::min(delta, 0.1f);
        const float decay = std::exp(-omega * frame_delta);
        const glm::vec3 offset = current_position - target_position;

        const glm::vec3 new_offset =
            (offset * (1.0f + omega * frame_delta) + camera_spring_velocity * frame_delta) * decay;
        camera_spring_velocity =
            (camera_spring_velocity * (1.0f - omega * frame_delta) -
             offset * (omega * omega * frame_delta)) * decay;

        return target_position + new_offset;
    }

    void followCamera(const glm::vec3& position, float delta, EntityRef target_entity)
    {
        CameraSpringComponent* spring = nullptr;
        if (game_world->isEntityValid(target_entity))
            spring = game_world->tryGetComponent<CameraSpringComponent>(target_entity);

        if (!spring || !spring->enabled)
        {
            resetCameraSpring();
            float camera_speed = 10.0f;
            float t = 1.0f - std::exp(-camera_speed * delta);
            game_world->world_camera.position = glm::mix(game_world->world_camera.position, position, t);
            return;
        }

        if (spring->rotate_target_yaw && game_world->hasComponent<TransformComponent>(target_entity))
        {
            auto& target_transform = *game_world->tryGetComponent<TransformComponent>(target_entity);
            target_transform.rotation.y = glm::degrees(game_world->world_camera.rotation.y);
        }

        glm::vec3 camera_rotation = game_world->world_camera.rotation;
        camera_rotation.x = std::clamp(camera_rotation.x, -1.5f, 1.5f);
        const glm::quat view_rotation = glm::quat(camera_rotation);
        const glm::vec3 forward = view_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 right = view_rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

        const glm::vec3 pivot = position + spring->pivot_offset;
        glm::vec3 desired_position =
            pivot +
            right * spring->local_offset.x +
            up * spring->local_offset.y +
            forward * spring->local_offset.z;
        desired_position = resolveCameraSpringCollision(pivot, desired_position, *spring, target_entity);

        const bool changed_target = camera_spring_entity != target_entity;
        const bool should_snap =
            !camera_spring_initialized ||
            changed_target ||
            (spring->snap_distance > 0.0f &&
             glm::distance(game_world->world_camera.position, desired_position) > spring->snap_distance);

        camera_spring_entity = target_entity;
        if (should_snap)
        {
            game_world->world_camera.position = desired_position;
            camera_spring_velocity = glm::vec3(0.0f);
            camera_spring_initialized = true;
            return;
        }

        game_world->world_camera.position = stepCameraSpring(
            game_world->world_camera.position, desired_position, delta, *spring);
    }

    CharacterMoveInput collectPlayerMoveInput(const LocalPlayerInputComponent& local_input) const
    {
        CharacterMoveInput input;
        input.move_forward = local_input.move.y;
        input.move_right = local_input.move.x;
        if (local_input.jump_held || local_input.jump_press_pending) input.buttons |= CharacterMoveFlags::Jump;
        if (local_input.sprint_held) input.buttons |= CharacterMoveFlags::Sprint;
        if (local_input.crouch_held) input.buttons |= CharacterMoveFlags::Crouch;
        input.camera_yaw = game_world->world_camera.rotation.y;
        input.camera_pitch = game_world->world_camera.rotation.x;
        return input;
    }

    void updatePlayer(float delta, uint64_t trace_frame_id, MovementTrace::FrameSummary& trace_frame)
    {
        trace_frame.reason = "transform_missing";
        if (!game_world->hasComponent<TransformComponent>(player_entity)) return;

        auto& trans = *game_world->tryGetComponent<TransformComponent>(player_entity);
        if (!local_player_look_applied)
        {
            if (const auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity))
                applyGamepadLook(*local_input, delta);
        }

        if (!movement_enabled)
        {
            trace_frame.reason = "movement_disabled";
            MovementTrace::emit("offline_suppressed", "role=client frame_scope=controller_update frame_id={} reason=movement_disabled owner=player pending=unavailable fixed_tick=unavailable", trace_frame_id);
            if (auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity))
                local_input->jump_press_pending = false;
            followCamera(trans.position, delta, player_entity);
            return;
        }

        if (game_world->hasComponent<CharacterControllerComponent>(player_entity))
        {
            auto& controller = *game_world->tryGetComponent<CharacterControllerComponent>(player_entity);
            auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity);
            bool input_enabled = controller.input_enabled;
            if (game_world->hasComponent<PlayerComponent>(player_entity))
                input_enabled = input_enabled && game_world->tryGetComponent<PlayerComponent>(player_entity)->input_enabled;
            if (!input_enabled || !local_input || !local_input->input_enabled)
            {
                trace_frame.reason = "input_disabled_or_missing";
                MovementTrace::emit("offline_suppressed", "role=client frame_scope=controller_update frame_id={} reason=input_disabled_or_missing controller_enabled={} local_present={} local_enabled={} pending={} fixed_tick=unavailable",
                    trace_frame_id, input_enabled, local_input != nullptr,
                    local_input ? (local_input->input_enabled ? "true" : "false") : "unavailable",
                    local_input ? (local_input->jump_press_pending ? "true" : "false") : "unavailable");
                if (local_input) local_input->jump_press_pending = false;
                return;
            }

            CharacterMoveInput input = collectPlayerMoveInput(*local_input);
            CharacterControllerState state;
            state.position = trans.position;
            const float step = game_world->fixed_delta > 0.0f ? game_world->fixed_delta : delta;
            character_delta_accumulator = std::min(character_delta_accumulator + std::max(delta, 0.0f), step * 5.0f);
            CharacterPhysicsService character_physics(*game_world);
            trace_frame.reason = "accumulator_below_step";
            while (character_delta_accumulator >= step)
            {
                character_delta_accumulator -= step;
                trace_frame.step();
                trace_frame.reason = "none";
                static std::atomic<uint64_t> diagnostic_step_counter{0};
                uint64_t diagnostic_step = 0;
                CharacterControllerState pre;
                const bool pre_available = MovementTrace::enabled() && character_physics.readControllerState(player_entity, pre);
                if (MovementTrace::enabled())
                {
                    diagnostic_step = diagnostic_step_counter.fetch_add(1, std::memory_order_relaxed) + 1;
                    const std::string pre_fields = pre_available
                        ? spdlog::fmt_lib::format("pre_vel={},{},{} pre_grounded={}", pre.velocity.x, pre.velocity.y, pre.velocity.z, pre.grounded)
                        : "pre_vel=unavailable pre_grounded=unavailable";
                    MovementTrace::emit("offline_step_begin", "role=client frame_scope=controller_update frame_id={} frame_step={} local_step={} sdl_step_ns={} sdl_clock=SDL_GetTicksNS fixed_delta={} held={} pending={} buttons={} forward={} right={} pre_available={} {}",
                        trace_frame_id, trace_frame.steps, diagnostic_step, SDL_GetTicksNS(), step, local_input->jump_held, local_input->jump_press_pending, input.buttons,
                        input.move_forward, input.move_right, pre_available, pre_fields);
                }
                const bool simulated = character_physics.simulateController(player_entity, input, step, state);
                if (MovementTrace::enabled())
                {
                    const std::string pre_fields = pre_available
                        ? spdlog::fmt_lib::format("pre_pos={},{},{} pre_vel={},{},{} pre_grounded={} pre_jump_held={} pre_coyote={} pre_buffer={}",
                            pre.position.x, pre.position.y, pre.position.z, pre.velocity.x, pre.velocity.y, pre.velocity.z,
                            pre.grounded, pre.jump_was_held, pre.coyote_timer, pre.jump_buffer_timer)
                        : "pre_pos=unavailable pre_vel=unavailable pre_grounded=unavailable pre_jump_held=unavailable pre_coyote=unavailable pre_buffer=unavailable";
                    const std::string post_fields = simulated
                        ? spdlog::fmt_lib::format("post_pos={},{},{} post_vel={},{},{} post_grounded={} jumped={} landed={} post_normal={},{},{} post_ground_vel={},{},{}",
                            state.position.x, state.position.y, state.position.z, state.velocity.x, state.velocity.y, state.velocity.z,
                            state.grounded, state.jumped, state.landed, state.ground_normal.x, state.ground_normal.y, state.ground_normal.z,
                            state.ground_velocity.x, state.ground_velocity.y, state.ground_velocity.z)
                        : "post_pos=unavailable post_vel=unavailable post_grounded=unavailable jumped=unavailable landed=unavailable post_normal=unavailable post_ground_vel=unavailable";
                    MovementTrace::emit("offline_step", "role=client frame_scope=controller_update frame_id={} frame_step={} local_step={} fixed_delta={} held={} pending={} buttons={} forward={} right={} pre_available={} success={} {} {}",
                        trace_frame_id, trace_frame.steps, diagnostic_step, step, local_input->jump_held, local_input->jump_press_pending, input.buttons,
                        input.move_forward, input.move_right, pre_available, simulated, pre_fields, post_fields);
                }
                if (!simulated)
                {
                    trace_frame.reason = "simulation_failed";
                    LOG_ENGINE_WARN("Failed to simulate local character controller");
                    break;
                }
                local_input->jump_press_pending = false;
                if (!local_input->jump_held)
                    input.buttons &= ~CharacterMoveFlags::Jump;
            }

            followCamera(state.position, delta, player_entity);
            return;
        }

        trace_frame.reason = "fallback_components_missing";
        if (!game_world->hasComponent<PlayerComponent>(player_entity) ||
            !game_world->hasComponent<RigidBodyComponent>(player_entity)) return;

        auto& pc = *game_world->tryGetComponent<PlayerComponent>(player_entity);
        auto& rb = *game_world->tryGetComponent<RigidBodyComponent>(player_entity);
        auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity);
        if (!pc.input_enabled || !local_input || !local_input->input_enabled)
        {
            trace_frame.reason = "input_disabled_or_missing";
            if (local_input) local_input->jump_press_pending = false;
            return;
        }

        CharacterMoveInput input = collectPlayerMoveInput(*local_input);

        CharacterControllerState current;
        current.position = trans.position;
        current.velocity = rb.velocity;
        current.grounded = pc.grounded;
        current.ground_normal = pc.ground_normal;

        CharacterController::MovementTuning tuning;
        tuning.max_speed = pc.speed;
        tuning.jump_velocity = pc.jump_force;
        tuning.gravity = game_world->getGravity() * game_world->getPhysicsSystem().getGravityAcceleration();
        tuning.fixed_delta = game_world->fixed_delta;
        tuning.stop_speed = std::max(pc.speed, 0.0f) * 0.25f;
        tuning.air_wish_speed_cap = std::max(pc.speed, 0.0f) * (30.0f / 320.0f);

        trace_frame.reason = "fallback_frame_simulation_not_fixed";
        const CharacterControllerState simulated =
            CharacterController::simulateSourceMovement(input, current, tuning);
        local_input->jump_press_pending = false;
        rb.velocity = simulated.velocity;
        trans.position = simulated.position;
        pc.grounded = simulated.grounded;
        pc.ground_normal = simulated.ground_normal;

        const glm::vec3 wish_dir = CharacterController::buildWishMove(input, tuning.max_speed).direction;
        LOG_ENGINE_TRACE("[PlayerCtrl] wish=({},{},{}) vel=({},{},{}) grounded={}",
            wish_dir.x, wish_dir.y, wish_dir.z,
            rb.velocity.x, rb.velocity.y, rb.velocity.z, pc.grounded);

        followCamera(trans.position, delta, player_entity);
    }

    void updateFreecam(float delta, uint64_t trace_frame_id, MovementTrace::FrameSummary& trace_frame)
    {
        trace_frame.reason = "freecam";
        if (!game_world->hasComponent<FreecamComponent>(freecam_entity) ||
            !game_world->hasComponent<TransformComponent>(freecam_entity)) return;
        resetCameraSpring();

        auto& fc = *game_world->tryGetComponent<FreecamComponent>(freecam_entity);
        auto& trans = *game_world->tryGetComponent<TransformComponent>(freecam_entity);
        auto* local_input = game_world->tryGetComponent<LocalPlayerInputComponent>(player_entity);

        MovementTrace::emit("offline_suppressed", "role=client frame_scope=controller_update frame_id={} reason=freecam local_present={} pending={} fixed_tick=unavailable",
            trace_frame_id, local_input != nullptr,
            local_input ? (local_input->jump_press_pending ? "true" : "false") : "unavailable");
        if (local_input) local_input->jump_press_pending = false;
        if (!fc.input_enabled || !local_input || !local_input->input_enabled) return;

        glm::vec3 local_movement = local_input->freecam_move;

        if (glm::length(local_movement) > 0)
            local_movement = glm::normalize(local_movement);

        float current_speed = local_input->freecam_fast
            ? fc.fast_movement_speed
            : fc.movement_speed;

        glm::vec3 world_movement = game_world->world_camera.camera_rot_quaternion() * local_movement;
        game_world->world_camera.position += world_movement * current_speed * delta;
        trans.position = game_world->world_camera.position;
    }

public:
    camera& getActiveCamera()
    {
        return game_world->world_camera;
    }

    bool isFreecamMode() const { return freecam_mode_enabled; }

    void setMovementEnabled(bool enabled) { movement_enabled = enabled; }
    bool isMovementEnabled() const { return movement_enabled; }
};
