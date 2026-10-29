#pragma once

// Opt in by setting GARDEN_MOVEMENT_TRACE to a run/scenario identifier.
// Each binary module has its own bounded budget; retain process logs separately.
#include "Utils/Log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace MovementTrace
{
    constexpr uint32_t schema = 2;
    constexpr uint32_t record_limit = 8192;

    enum class BudgetDecision { Record, Exhausted, Drop };
    struct Budget
    {
        std::atomic<uint32_t> count{0};
        std::atomic<bool> reported{false};
        BudgetDecision next(uint32_t limit)
        {
            uint32_t value = count.load(std::memory_order_relaxed);
            while (value < limit)
                if (count.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return BudgetDecision::Record;
            return reported.exchange(true, std::memory_order_relaxed)
                ? BudgetDecision::Drop : BudgetDecision::Exhausted;
        }
    };

    struct FrameSummary
    {
        uint32_t steps = 0;
        const char* reason = "none";
        void step() { ++steps; }
        const char* category() const { return steps == 0 ? "zero" : steps == 1 ? "one" : "multiple"; }
    };

    inline const char* runId()
    {
        static const char* id = std::getenv("GARDEN_MOVEMENT_TRACE");
        return id && *id ? id : nullptr;
    }

    inline bool enabled() { return runId() != nullptr; }

    // Acceptance-only scoped tracing keeps the epoch/command stream visible
    // across a long competitive transition without flooding the bounded budget
    // with render-frame and prediction records.
    inline bool admissionScope()
    {
        static const char* scope = std::getenv("GARDEN_ADMISSION_TRACE");
        return scope && std::strcmp(scope, "1") == 0;
    }

    // Focused physical movement investigation; enable alongside GARDEN_MOVEMENT_TRACE.
    inline bool movementScope()
    {
        static const char* scope = std::getenv("GARDEN_MOVEMENT_TRACE_SCOPE");
        return scope && std::strcmp(scope, "mc0") == 0;
    }

    inline bool movementStage(const char* stage)
    {
        static constexpr const char* stages[] = {
            "key_received", "key_routing", "physical_key", "key_state",
            "action_frame", "client_frame_begin", "client_frame", "client_step_begin",
            "offline_frame_begin", "offline_frame", "offline_suppressed",
            "offline_step_begin", "offline_step", "client_command", "client_prediction",
            "client_prediction_suppressed", "client_admission", "server_admission",
            "authority_received", "authority_decision", "authority_rejected_state", "authority_step"
        };
        for (const char* candidate : stages)
            if (std::strcmp(stage, candidate) == 0) return true;
        return false;
    }

    inline bool admissionStage(const char* stage)
    {
        return std::strcmp(stage, "client_command") == 0 ||
               std::strcmp(stage, "client_admission") == 0 ||
               std::strcmp(stage, "server_admission") == 0 ||
               std::strcmp(stage, "authority_rejected_state") == 0 ||
               std::strcmp(stage, "authority_received") == 0 ||
               std::strcmp(stage, "authority_decision") == 0 ||
               std::strcmp(stage, "authority_step") == 0;
    }

    inline bool acceptsStage(const char* stage)
    {
        if (movementScope()) return movementStage(stage);
        return !admissionScope() || admissionStage(stage);
    }

    inline void emitRecord(const char* stage, const std::string& fields)
    {
        static Budget budget;
        static std::atomic<bool> manifest_emitted{false};
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!manifest_emitted.exchange(true, std::memory_order_relaxed))
            LOG_ENGINE_INFO("movement_trace schema={} run={} stage=manifest monotonic_ns={} clock=steady_process clock_resolution=unavailable sdl_event_and_step_clock=SDL_GetTicksNS sdl_resolution=unavailable sdl_to_steady_mapping=unavailable client_server_clock_mapping=unavailable cross_process_clock_mapping=unavailable frame_scope=per_stream controller_and_game_frame_ids=independent frame_id_zero=unavailable physical_event_frame=unavailable correction=unavailable replay=unavailable acknowledgement=unavailable stance=unavailable tuning_identity=unavailable view=command_and_authority_radians ground_normal_and_velocity=stage_dependent prior_held_and_timers=offline_only input_tick=stage_dependent fixed_tick=stage_dependent process_identity=external_log_source environment_manifest=external_required scope={} limit={}",
                            schema, runId(), ns, movementScope() ? "mc0" : admissionScope() ? "admission" : "default",
                            movementScope() ? 1000000u : admissionScope() ? 100000u : record_limit);
        const uint32_t limit = movementScope() ? 1000000u : admissionScope() ? 100000u : record_limit;
        const auto decision = budget.next(limit);
        if (decision == BudgetDecision::Drop) return;
        if (decision == BudgetDecision::Exhausted)
        {
            LOG_ENGINE_INFO("movement_trace schema={} run={} stage=budget_exhausted monotonic_ns={} limit={} further_records=dropped",
                            schema, runId(), ns, limit);
            return;
        }
        LOG_ENGINE_INFO("movement_trace schema={} run={} stage={} monotonic_ns={} clock=steady_process {}",
                        schema, runId(), stage, ns, fields);
    }

    template<typename... Args>
    inline void emit(const char* stage, spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (!enabled() || !acceptsStage(stage)) return;
        emitRecord(stage, spdlog::fmt_lib::format(format, std::forward<Args>(args)...));
    }
}
