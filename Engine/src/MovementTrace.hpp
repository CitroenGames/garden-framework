#pragma once

// Opt in by setting GARDEN_MOVEMENT_TRACE to a run/scenario identifier.
// Each binary module has its own bounded budget; retain process logs separately.
#include "Utils/Log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
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

    inline void emitRecord(const char* stage, const std::string& fields)
    {
        static Budget budget;
        static std::atomic<bool> manifest_emitted{false};
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!manifest_emitted.exchange(true, std::memory_order_relaxed))
            LOG_ENGINE_INFO("movement_trace schema={} run={} stage=manifest monotonic_ns={} clock=steady_process clock_resolution=unavailable sdl_event_and_step_clock=SDL_GetTicksNS sdl_resolution=unavailable sdl_to_steady_mapping=unavailable client_server_clock_mapping=unavailable cross_process_clock_mapping=unavailable frame_scope=per_stream controller_and_game_frame_ids=independent frame_id_zero=unavailable physical_event_frame=unavailable correction=unavailable replay=unavailable acknowledgement=unavailable stance=unavailable tuning_identity=unavailable view=command_and_authority_radians ground_normal_and_velocity=stage_dependent prior_held_and_timers=offline_only input_tick=stage_dependent fixed_tick=stage_dependent process_identity=external_log_source environment_manifest=external_required",
                            schema, runId(), ns);
        const auto decision = budget.next(record_limit);
        if (decision == BudgetDecision::Drop) return;
        if (decision == BudgetDecision::Exhausted)
        {
            LOG_ENGINE_INFO("movement_trace schema={} run={} stage=budget_exhausted monotonic_ns={} limit={} further_records=dropped",
                            schema, runId(), ns, record_limit);
            return;
        }
        LOG_ENGINE_INFO("movement_trace schema={} run={} stage={} monotonic_ns={} clock=steady_process {}",
                        schema, runId(), stage, ns, fields);
    }

    template<typename... Args>
    inline void emit(const char* stage, spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (!enabled()) return;
        emitRecord(stage, spdlog::fmt_lib::format(format, std::forward<Args>(args)...));
    }
}
