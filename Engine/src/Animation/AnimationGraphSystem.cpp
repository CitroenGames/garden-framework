#include "AnimationGraphSystem.hpp"

#include "Animation/AnimationGraph.hpp"
#include "Animation/AnimationSampling.hpp"
#include "Components/AnimGraphComponent.hpp"
#include "Components/AnimGraphStateComponent.hpp"
#include "Components/AnimPoseComponent.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace
{
bool initialize(const AnimGraph& graph, AnimGraphStateComponent& state)
{
    if (state.initialized && state.initialized_graph == &graph &&
        state.parameters.size() == graph.parameters().size() &&
        state.machines.size() == graph.machines().size())
        return true;

    state.parameters.clear();
    state.parameters.reserve(graph.parameters().size());
    for (const auto& parameter : graph.parameters()) state.parameters.push_back(parameter.default_value);
    state.machines.clear();
    state.machines.reserve(graph.machines().size());
    for (const auto& machine : graph.machines()) state.machines.push_back({machine.entry_state, 0.0f});
    state.initialized_graph = &graph;
    state.initialized = true;
    return true;
}

bool validState(const AnimGraph& graph, const AnimGraphMachineState& runtime)
{
    return runtime.active_state < graph.states().size();
}

float sampleTime(const AnimGraphStateDefinition& definition, const AnimationClip& clip, float state_time)
{
    const float duration = clip.duration();
    if (duration <= 0.0f || !std::isfinite(duration) || !std::isfinite(state_time)) return 0.0f;
    const float scaled = std::max(0.0f, state_time * definition.speed);
    if (!definition.looping) return std::min(scaled, duration);
    const float wrapped = std::fmod(scaled, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

template<typename T>
bool setValue(entt::registry& registry, entt::entity entity, std::string_view parameter, T value,
              AnimGraphParameterType expected)
{
    if (!registry.valid(entity)) return false;
    const auto* binding = registry.try_get<AnimGraphComponent>(entity);
    auto* runtime = registry.try_get<AnimGraphStateComponent>(entity);
    if (!binding || !binding->graph || !runtime) return false;
    // Schema reset is owned by advance(), the fixed-tick safe boundary. A
    // gameplay write between a graph replacement and that boundary therefore
    // cannot partially initialize a new instance at presentation frequency.
    if (!runtime->initialized || runtime->initialized_graph != binding->graph.get()) return false;
    const int index = binding->graph->parameterIndex(parameter);
    if (index < 0 || binding->graph->parameters()[static_cast<size_t>(index)].type != expected) return false;
    if constexpr (std::is_same_v<T, float>)
        if (!std::isfinite(value)) return false;
    if constexpr (std::is_same_v<T, glm::vec3>)
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
    runtime->parameters[static_cast<size_t>(index)] = value;
    return true;
}
}

namespace AnimGraphSystem
{
void advance(entt::registry& registry, float fixed_dt)
{
    auto view = registry.view<AnimGraphComponent, AnimGraphStateComponent>();
    for (const entt::entity entity : view)
    {
        const auto& binding = view.get<AnimGraphComponent>(entity);
        auto& runtime = view.get<AnimGraphStateComponent>(entity);
        if (!binding.graph)
        {
            runtime = {};
            continue;
        }
        initialize(*binding.graph, runtime);
        if (!std::isfinite(fixed_dt) || fixed_dt <= 0.0f) continue;
        for (auto& machine : runtime.machines)
        {
            if (!validState(*binding.graph, machine)) continue;
            const float current = std::isfinite(machine.state_time) ? machine.state_time : 0.0f;
            const float next = current + fixed_dt;
            // Reset rather than retain an unrepresentable or stalled clock.
            // A very large finite float can round a positive increment back to
            // itself before it reaches infinity. Treat that as overflow too:
            // retaining it would silently stop fixed-tick advancement. Clip
            // sampling is periodic for looping states and deterministic
            // rest/zero for non-looping states, so reset cannot inject a NaN
            // into a pose.
            machine.state_time = std::isfinite(next) && next > current ? next : 0.0f;
        }
    }
}

void evaluate(entt::registry& registry, bool build_skinning_palette)
{
    auto view = registry.view<AnimGraphComponent, AnimGraphStateComponent>();
    for (const entt::entity entity : view)
    {
        const auto& binding = view.get<AnimGraphComponent>(entity);
        auto& runtime = view.get<AnimGraphStateComponent>(entity);
        auto& pose = registry.get_or_emplace<AnimPoseComponent>(entity);
        pose.pose_valid = false;
        pose.bone_matrices.clear();
        if (!binding.graph) continue;
        const Skeleton* skeleton = binding.graph->resources().skeleton();
        if (!skeleton) continue;

        AnimationSampling::makeRestPose(*skeleton, pose.local_pose);
        // Foundation has one independently evaluated state source per machine.
        // Later layers supply composition; the base machine is the first one.
        if (runtime.initialized && runtime.initialized_graph == binding.graph.get() &&
            !runtime.machines.empty() && validState(*binding.graph, runtime.machines.front()))
        {
            const auto& state = binding.graph->states()[runtime.machines.front().active_state];
            if (state.source == AnimGraphPoseSource::Clip)
            {
                const auto clip = binding.graph->resources().findClip(state.clip_name);
                if (clip)
                    AnimationSampling::overlayClip(*clip, sampleTime(state, *clip, runtime.machines.front().state_time),
                                                   pose.local_pose);
            }
        }
        if (build_skinning_palette) pose.local_pose.toMatrices(pose.bone_matrices);
        pose.pose_valid = true;
    }
}

bool setBool(entt::registry& registry, entt::entity entity, std::string_view parameter, bool value)
{
    return setValue(registry, entity, parameter, value, AnimGraphParameterType::Bool);
}

bool setInt(entt::registry& registry, entt::entity entity, std::string_view parameter, int32_t value)
{
    return setValue(registry, entity, parameter, value, AnimGraphParameterType::Int);
}

bool setFloat(entt::registry& registry, entt::entity entity, std::string_view parameter, float value)
{
    return setValue(registry, entity, parameter, value, AnimGraphParameterType::Float);
}

bool setVec3(entt::registry& registry, entt::entity entity, std::string_view parameter, const glm::vec3& value)
{
    return setValue(registry, entity, parameter, value, AnimGraphParameterType::Vec3);
}

int activeState(const entt::registry& registry, entt::entity entity, std::string_view machine)
{
    if (!registry.valid(entity)) return -1;
    const auto* binding = registry.try_get<AnimGraphComponent>(entity);
    const auto* runtime = registry.try_get<AnimGraphStateComponent>(entity);
    if (!binding || !binding->graph || !runtime || !runtime->initialized ||
        runtime->initialized_graph != binding->graph.get()) return -1;
    const int machine_index = binding->graph->machineIndex(machine);
    if (machine_index < 0 || static_cast<size_t>(machine_index) >= runtime->machines.size()) return -1;
    const uint32_t state = runtime->machines[static_cast<size_t>(machine_index)].active_state;
    return state < binding->graph->states().size() ? static_cast<int>(state) : -1;
}
} // namespace AnimGraphSystem
