#pragma once

#include "Network/NetworkTypes.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace FpsServer {

struct LagCompensationTelemetry
{
    uint64_t target_history_sample_misses = 0;
};

struct LagCompensationTarget
{
    entt::entity entity = entt::null;
    uint32_t network_id = 0;
    uint16_t owner_client_id = 0;
    glm::vec3 current_position{0.0f};
};

struct LagCompensatedTarget
{
    entt::entity entity = entt::null;
    uint32_t network_id = 0;
    uint16_t owner_client_id = 0;
    glm::vec3 position{0.0f};
};

// Call once when preparing the eligible targets for a shot, then reuse the
// returned poses for every pellet. A missing entity history falls back to its
// current transform and contributes exactly one miss for that target/shot.
template <typename SampleFn>
std::vector<LagCompensatedTarget> prepareLagCompensatedTargetsForShot(
    const std::vector<LagCompensationTarget>& targets,
    uint32_t view_tick,
    SampleFn&& sample_entity,
    LagCompensationTelemetry& telemetry)
{
    std::vector<LagCompensatedTarget> resolved;
    resolved.reserve(targets.size());
    for (const LagCompensationTarget& target : targets) {
        Net::ComponentSnapshot snapshot;
        glm::vec3 target_position = target.current_position;
        if (sample_entity(target.network_id, view_tick, snapshot)) {
            target_position = snapshot.position;
        } else {
            ++telemetry.target_history_sample_misses;
        }

        resolved.push_back({target.entity, target.network_id, target.owner_client_id, target_position});
    }
    return resolved;
}

} // namespace FpsServer
