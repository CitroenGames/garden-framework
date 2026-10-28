#include "AnimationGraphSystem.hpp"

#include "Animation/AnimationGraph.hpp"
#include "Animation/AnimationSampling.hpp"
#include "Animation/Pose.hpp"
#include "Components/AnimGraphComponent.hpp"
#include "Components/AnimGraphStateComponent.hpp"
#include "Components/AnimPoseComponent.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"

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
    for (const auto& machine : graph.machines())
        state.machines.push_back({machine.entry_state, 0.0f, UINT32_MAX, 0.0f, 0.0f, 0.0f});
    state.initialized_graph = &graph;
    state.initialized = true;
    return true;
}

bool validState(const AnimGraph& graph, uint32_t state)
{
    return state < graph.states().size();
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

float advanceClock(float current, float fixed_dt)
{
    current = std::isfinite(current) && current >= 0.0f ? current : 0.0f;
    const float next = current + fixed_dt;
    return std::isfinite(next) && next > current ? next : 0.0f;
}

bool conditionSatisfied(const AnimGraphCondition& condition, const AnimGraphParameterValue& current)
{
    switch (condition.op)
    {
    case AnimGraphConditionOperator::IsTrue: return std::get<bool>(current);
    case AnimGraphConditionOperator::Equal:
        if (const auto* value = std::get_if<bool>(&current)) return *value == std::get<bool>(condition.value);
        if (const auto* value = std::get_if<int32_t>(&current)) return *value == std::get<int32_t>(condition.value);
        if (const auto* value = std::get_if<float>(&current)) return *value == std::get<float>(condition.value);
        return std::get<glm::vec3>(current) == std::get<glm::vec3>(condition.value);
    case AnimGraphConditionOperator::NotEqual:
        if (const auto* value = std::get_if<bool>(&current)) return *value != std::get<bool>(condition.value);
        if (const auto* value = std::get_if<int32_t>(&current)) return *value != std::get<int32_t>(condition.value);
        if (const auto* value = std::get_if<float>(&current)) return *value != std::get<float>(condition.value);
        return std::get<glm::vec3>(current) != std::get<glm::vec3>(condition.value);
    case AnimGraphConditionOperator::Less:
        if (const auto* value = std::get_if<int32_t>(&current)) return *value < std::get<int32_t>(condition.value);
        return std::get<float>(current) < std::get<float>(condition.value);
    case AnimGraphConditionOperator::LessEqual:
        if (const auto* value = std::get_if<int32_t>(&current)) return *value <= std::get<int32_t>(condition.value);
        return std::get<float>(current) <= std::get<float>(condition.value);
    case AnimGraphConditionOperator::Greater:
        if (const auto* value = std::get_if<int32_t>(&current)) return *value > std::get<int32_t>(condition.value);
        return std::get<float>(current) > std::get<float>(condition.value);
    case AnimGraphConditionOperator::GreaterEqual:
        if (const auto* value = std::get_if<int32_t>(&current)) return *value >= std::get<int32_t>(condition.value);
        return std::get<float>(current) >= std::get<float>(condition.value);
    }
    return false;
}

bool eligible(const AnimGraph& graph, const AnimGraphMachineState& machine,
              const AnimGraphTransitionDefinition& transition,
              const std::vector<AnimGraphParameterValue>& parameters)
{
    if (transition.from_state != machine.active_state) return false;
    const auto& source = graph.states()[machine.active_state];
    if (transition.exit_time >= 0.0f)
    {
        const auto clip = graph.resources().findClip(source.clip_name);
        if (!clip || source.source != AnimGraphPoseSource::Clip || clip->duration() <= 0.0f ||
            sampleTime(source, *clip, machine.state_time) / clip->duration() < transition.exit_time)
            return false;
    }
    for (const auto& condition : transition.conditions)
    {
        const int index = graph.parameterIndex(condition.parameter);
        if (index < 0 || static_cast<size_t>(index) >= parameters.size() ||
            !conditionSatisfied(condition, parameters[static_cast<size_t>(index)])) return false;
    }
    return true;
}

void queueNotifies(const AnimGraph& graph, entt::entity entity, uint32_t state_index,
                   float previous_time, float current_time)
{
    if (!validState(graph, state_index)) return;
    const auto& state = graph.states()[state_index];
    if (state.source != AnimGraphPoseSource::Clip || current_time <= previous_time || state.speed <= 0.0f) return;
    const auto clip = graph.resources().findClip(state.clip_name);
    if (!clip || clip->duration() <= 0.0f) return;
    const float previous = std::max(0.0f, previous_time * state.speed) / clip->duration();
    const float current = std::max(0.0f, current_time * state.speed) / clip->duration();
    for (const auto& notify : graph.notifies())
    {
        if (notify.clip_name != state.clip_name) continue;
        if (!state.looping)
        {
            if (notify.normalized_time > previous && notify.normalized_time <= std::min(current, 1.0f))
                EventBus::get().queue(AnimationNotifyEvent{entity, notify.event_name,
                    static_cast<int>(state_index), notify.normalized_time});
            continue;
        }
        float crossing = notify.normalized_time;
        while (crossing <= previous) crossing += 1.0f;
        while (crossing <= current)
        {
            EventBus::get().queue(AnimationNotifyEvent{entity, notify.event_name,
                static_cast<int>(state_index), notify.normalized_time});
            crossing += 1.0f;
        }
    }
}

void sampleState(const AnimGraph& graph, uint32_t state_index, float state_time, Pose& pose)
{
    const Skeleton* skeleton = graph.resources().skeleton();
    if (!skeleton || !validState(graph, state_index)) return;
    AnimationSampling::makeRestPose(*skeleton, pose);
    const auto& state = graph.states()[state_index];
    if (state.source != AnimGraphPoseSource::Clip) return;
    const auto clip = graph.resources().findClip(state.clip_name);
    if (clip) AnimationSampling::overlayClip(*clip, sampleTime(state, *clip, state_time), pose);
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
        for (size_t machine_index = 0; machine_index < runtime.machines.size(); ++machine_index)
        {
            auto& machine = runtime.machines[machine_index];
            if (!validState(*binding.graph, machine.active_state)) continue;
            const float previous_time = machine.state_time;
            machine.state_time = advanceClock(machine.state_time, fixed_dt);
            if (validState(*binding.graph, machine.fading_from_state))
                machine.fading_from_time = advanceClock(machine.fading_from_time, fixed_dt);
            if (machine.fade_duration > 0.0f)
            {
                machine.fade_elapsed = advanceClock(machine.fade_elapsed, fixed_dt);
                if (machine.fade_elapsed >= machine.fade_duration)
                {
                    machine.fading_from_state = UINT32_MAX;
                    machine.fading_from_time = 0.0f;
                    machine.fade_elapsed = 0.0f;
                    machine.fade_duration = 0.0f;
                }
            }

            queueNotifies(*binding.graph, entity, machine.active_state, previous_time, machine.state_time);
            for (const auto& transition : binding.graph->transitions())
            {
                if (!eligible(*binding.graph, machine, transition, runtime.parameters)) continue;
                const uint32_t from_state = machine.active_state;
                const float from_time = machine.state_time;
                machine.active_state = transition.to_state;
                machine.state_time = 0.0f;
                if (transition.blend_duration <= 0.0f)
                {
                    machine.fading_from_state = UINT32_MAX;
                    machine.fading_from_time = 0.0f;
                    machine.fade_elapsed = 0.0f;
                    machine.fade_duration = 0.0f;
                }
                else
                {
                    // A later transition fades from the previous destination;
                    // graph data stays immutable and never owns a pose snapshot.
                    machine.fading_from_state = from_state;
                    machine.fading_from_time = from_time;
                    machine.fade_elapsed = 0.0f;
                    machine.fade_duration = transition.blend_duration;
                }
                EventBus::get().queue(AnimationStateChangedEvent{entity,
                    binding.graph->machines()[machine_index].name,
                    binding.graph->states()[from_state].name,
                    binding.graph->states()[transition.to_state].name});
                break;
            }
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
        if (runtime.initialized && runtime.initialized_graph == binding.graph.get() &&
            !runtime.machines.empty() && validState(*binding.graph, runtime.machines.front().active_state))
        {
            const auto& machine = runtime.machines.front();
            sampleState(*binding.graph, machine.active_state, machine.state_time, pose.local_pose);
            if (validState(*binding.graph, machine.fading_from_state) && machine.fade_duration > 0.0f)
            {
                Pose from_pose;
                sampleState(*binding.graph, machine.fading_from_state, machine.fading_from_time, from_pose);
                const float factor = std::clamp(machine.fade_elapsed / machine.fade_duration, 0.0f, 1.0f);
                pose.local_pose = Pose::blend(from_pose, pose.local_pose, factor);
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
    return validState(*binding->graph, state) ? static_cast<int>(state) : -1;
}
} // namespace AnimGraphSystem
