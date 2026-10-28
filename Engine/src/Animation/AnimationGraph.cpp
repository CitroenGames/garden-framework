#include "AnimationGraph.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace
{
bool valueMatches(AnimGraphParameterType type, const AnimGraphParameterValue& value)
{
    switch (type)
    {
    case AnimGraphParameterType::Bool: return std::holds_alternative<bool>(value);
    case AnimGraphParameterType::Int: return std::holds_alternative<int32_t>(value);
    case AnimGraphParameterType::Float: return std::holds_alternative<float>(value);
    case AnimGraphParameterType::Vec3: return std::holds_alternative<glm::vec3>(value);
    }
    return false;
}

bool finiteValue(const AnimGraphParameterValue& value)
{
    if (const auto* number = std::get_if<float>(&value)) return std::isfinite(*number);
    if (const auto* vector = std::get_if<glm::vec3>(&value))
        return std::isfinite(vector->x) && std::isfinite(vector->y) && std::isfinite(vector->z);
    return true;
}

bool conditionAllowsType(AnimGraphConditionOperator op, AnimGraphParameterType type)
{
    switch (op)
    {
    case AnimGraphConditionOperator::Equal:
    case AnimGraphConditionOperator::NotEqual:
        return true;
    case AnimGraphConditionOperator::Less:
    case AnimGraphConditionOperator::LessEqual:
    case AnimGraphConditionOperator::Greater:
    case AnimGraphConditionOperator::GreaterEqual:
        return type == AnimGraphParameterType::Int || type == AnimGraphParameterType::Float;
    case AnimGraphConditionOperator::IsTrue:
        return type == AnimGraphParameterType::Bool;
    }
    return false;
}

void add(AnimGraphValidationReport& report, const char* code, std::string message)
{
    report.errors.push_back({code, std::move(message)});
}
}

int AnimGraph::parameterIndex(std::string_view name) const
{
    for (size_t index = 0; index < m_definition.parameters.size(); ++index)
        if (m_definition.parameters[index].name == name) return static_cast<int>(index);
    return -1;
}

int AnimGraph::machineIndex(std::string_view name) const
{
    for (size_t index = 0; index < m_definition.machines.size(); ++index)
        if (m_definition.machines[index].name == name) return static_cast<int>(index);
    return -1;
}

namespace AnimGraphValidation
{
class GraphBuilder final
{
public:
    static std::shared_ptr<const AnimGraph> publish(AnimGraphDefinition definition,
                                                     std::shared_ptr<const AnimationResourceSet> resources)
    {
        return std::shared_ptr<const AnimGraph>(
            new AnimGraph(std::move(definition), std::move(resources)));
    }
};

std::shared_ptr<const AnimGraph> makeGraph(AnimGraphDefinition definition,
                                           std::shared_ptr<const AnimationResourceSet> resources,
                                           AnimGraphValidationReport& report)
{
    report.clear();
    if (!resources)
    {
        add(report, "ANIM_GRAPH_RESOURCE", "graph requires an immutable animation resource set");
        return nullptr;
    }
    if (definition.resource_signature.empty() ||
        definition.resource_signature != resources->sourceSignature())
        add(report, "ANIM_GRAPH_RESOURCE_SIGNATURE",
            "graph resource signature must match its immutable animation resource set");

    std::unordered_set<std::string> parameter_names;
    for (const auto& parameter : definition.parameters)
    {
        if (parameter.name.empty() || !parameter_names.emplace(parameter.name).second)
            add(report, "ANIM_GRAPH_PARAMETER_NAME", "parameter names must be non-empty and unique");
        if (!valueMatches(parameter.type, parameter.default_value))
            add(report, "ANIM_GRAPH_PARAMETER_TYPE", "parameter default value does not match its declared type");
        else if (!finiteValue(parameter.default_value))
            add(report, "ANIM_GRAPH_PARAMETER_NONFINITE", "parameter default value must be finite");
    }

    if (definition.machines.empty())
        add(report, "ANIM_GRAPH_MACHINE_COUNT", "graph requires at least one state machine");
    if (definition.machines.size() > 1)
        add(report, "ANIM_GRAPH_MACHINE_FOUNDATION",
            "the AN-3 foundation supports one base machine until layer composition is implemented");

    std::unordered_set<std::string> machine_names;
    for (const auto& machine : definition.machines)
        if (machine.name.empty() || !machine_names.emplace(machine.name).second)
            add(report, "ANIM_GRAPH_MACHINE_NAME", "machine names must be non-empty and unique");

    std::vector<std::unordered_set<std::string>> state_names(definition.machines.size());
    std::vector<bool> machine_has_state(definition.machines.size(), false);
    for (size_t state_index = 0; state_index < definition.states.size(); ++state_index)
    {
        const auto& state = definition.states[state_index];
        if (state.machine_index >= definition.machines.size())
        {
            add(report, "ANIM_GRAPH_STATE_MACHINE", "state references a machine outside the graph");
            continue;
        }
        machine_has_state[state.machine_index] = true;
        if (state.name.empty() || !state_names[state.machine_index].emplace(state.name).second)
            add(report, "ANIM_GRAPH_STATE_NAME", "state names must be non-empty and unique per machine");
        if (!std::isfinite(state.speed) || state.speed < 0.0f)
            add(report, "ANIM_GRAPH_STATE_SPEED", "state speed must be finite and non-negative");
        if (state.source == AnimGraphPoseSource::RestPose)
        {
            if (!state.clip_name.empty())
                add(report, "ANIM_GRAPH_REST_CLIP", "rest-pose states cannot reference a clip");
        }
        else if (state.source == AnimGraphPoseSource::Clip)
        {
            if (state.clip_name.empty() || !resources->findClip(state.clip_name))
                add(report, "ANIM_GRAPH_CLIP", "clip state references a missing clip");
        }
        else
        {
            add(report, "ANIM_GRAPH_STATE_SOURCE", "state uses an unsupported pose source");
        }
    }

    for (size_t machine_index = 0; machine_index < definition.machines.size(); ++machine_index)
    {
        const auto& machine = definition.machines[machine_index];
        if (!machine_has_state[machine_index])
            add(report, "ANIM_GRAPH_MACHINE_STATES", "every machine must own at least one state");
        if (machine.entry_state >= definition.states.size() ||
            (machine.entry_state < definition.states.size() &&
             definition.states[machine.entry_state].machine_index != machine_index))
            add(report, "ANIM_GRAPH_ENTRY_STATE", "machine entry state must belong to that machine");
    }

    for (const auto& transition : definition.transitions)
    {
        if (transition.from_state >= definition.states.size() || transition.to_state >= definition.states.size())
        {
            add(report, "ANIM_GRAPH_TRANSITION_STATE", "transition references a state outside the graph");
        }
        else if (definition.states[transition.from_state].machine_index !=
                 definition.states[transition.to_state].machine_index)
        {
            add(report, "ANIM_GRAPH_TRANSITION_MACHINE", "transition states must belong to the same machine");
        }
        if (!std::isfinite(transition.blend_duration) || transition.blend_duration < 0.0f)
            add(report, "ANIM_GRAPH_TRANSITION_BLEND", "transition blend duration must be finite and non-negative");
        if (!std::isfinite(transition.exit_time) ||
            (transition.exit_time != -1.0f && (transition.exit_time < 0.0f || transition.exit_time > 1.0f)))
            add(report, "ANIM_GRAPH_TRANSITION_EXIT", "transition exit time must be -1 or a normalized value");
        else if (transition.exit_time >= 0.0f && transition.from_state < definition.states.size() &&
                 definition.states[transition.from_state].source != AnimGraphPoseSource::Clip)
            add(report, "ANIM_GRAPH_TRANSITION_EXIT_SOURCE", "normalized exit time requires a clip source state");
        for (const auto& condition : transition.conditions)
        {
            const auto parameter = std::find_if(definition.parameters.begin(), definition.parameters.end(),
                [&condition](const AnimGraphParameterDefinition& candidate) { return candidate.name == condition.parameter; });
            if (condition.parameter.empty() || parameter == definition.parameters.end())
            {
                add(report, "ANIM_GRAPH_CONDITION_PARAMETER", "transition condition references a missing parameter");
                continue;
            }
            if (!conditionAllowsType(condition.op, parameter->type))
                add(report, "ANIM_GRAPH_CONDITION_OPERATOR", "transition condition operator does not support the parameter type");
            if (!valueMatches(parameter->type, condition.value))
                add(report, "ANIM_GRAPH_CONDITION_TYPE", "transition condition value does not match its parameter type");
            else if (!finiteValue(condition.value))
                add(report, "ANIM_GRAPH_CONDITION_NONFINITE", "transition condition value must be finite");
        }
    }

    for (const auto& notify : definition.notifies)
    {
        if (notify.clip_name.empty() || !resources->findClip(notify.clip_name))
            add(report, "ANIM_GRAPH_NOTIFY_CLIP", "notify references a missing clip");
        if (notify.event_name.empty())
            add(report, "ANIM_GRAPH_NOTIFY_NAME", "notify event name must be non-empty");
        if (!std::isfinite(notify.normalized_time) || notify.normalized_time < 0.0f || notify.normalized_time > 1.0f)
            add(report, "ANIM_GRAPH_NOTIFY_TIME", "notify time must be finite and normalized");
    }

    if (!report.valid()) return nullptr;
    return GraphBuilder::publish(std::move(definition), std::move(resources));
}
} // namespace AnimGraphValidation
