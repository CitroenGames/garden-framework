#pragma once

#include "Animation/AnimationResourceSet.hpp"
#include "EngineExport.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Graph definitions are decoded/cooked data. They are deliberately separate
// from the immutable published graph so malformed authored data cannot leak
// into runtime instances.
enum class AnimGraphParameterType : uint8_t
{
    Bool,
    Int,
    Float,
    Vec3,
};

using AnimGraphParameterValue = std::variant<bool, int32_t, float, glm::vec3>;

struct AnimGraphParameterDefinition
{
    std::string name;
    AnimGraphParameterType type = AnimGraphParameterType::Bool;
    AnimGraphParameterValue default_value = false;
};

// AN-3 v1 admits the source forms needed for deterministic state machines.
// Blend/Additive nodes are a later graph contract and are rejected here.
enum class AnimGraphPoseSource : uint8_t
{
    RestPose,
    Clip,
};

struct AnimGraphStateDefinition
{
    std::string name;
    uint32_t machine_index = 0;
    AnimGraphPoseSource source = AnimGraphPoseSource::RestPose;
    std::string clip_name;
    float speed = 1.0f;
    bool looping = true;
};

struct AnimGraphMachineDefinition
{
    std::string name;
    uint32_t entry_state = 0;
};

enum class AnimGraphConditionOperator : uint8_t
{
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    IsTrue,
};

struct AnimGraphCondition
{
    std::string parameter;
    AnimGraphConditionOperator op = AnimGraphConditionOperator::IsTrue;
    AnimGraphParameterValue value = false;
};

// Kept in authored table order. Runtime examines this table linearly so the
// first eligible transition is the sole transition that a machine may take in
// a fixed tick.
struct AnimGraphTransitionDefinition
{
    uint32_t from_state = UINT32_MAX;
    uint32_t to_state = UINT32_MAX;
    float blend_duration = 0.2f;
    float exit_time = -1.0f;
    std::vector<AnimGraphCondition> conditions;
};

// Notify names are opaque engine data. The graph never interprets them.
struct AnimGraphNotifyDefinition
{
    std::string clip_name;
    float normalized_time = 0.0f;
    std::string event_name;
};

struct AnimGraphDefinition
{
    // The resource signature is the graph's skeleton/resource owner binding.
    // It prevents a graph decoded for one immutable resource set from being
    // attached to an unrelated skeleton set.
    std::string resource_signature;
    std::vector<AnimGraphParameterDefinition> parameters;
    std::vector<AnimGraphStateDefinition> states;
    // The data shape remains a table for the later layered graph package; the
    // foundation validates exactly one base machine so it never drops a pose.
    std::vector<AnimGraphMachineDefinition> machines;
    std::vector<AnimGraphTransitionDefinition> transitions;
    std::vector<AnimGraphNotifyDefinition> notifies;
};

struct AnimGraphValidationError
{
    std::string code;
    std::string message;
};

struct AnimGraphValidationReport
{
    std::vector<AnimGraphValidationError> errors;

    bool valid() const { return errors.empty(); }
    void clear() { errors.clear(); }
};

namespace AnimGraphValidation
{
class GraphBuilder;
}

// Immutable graph resource. Runtime instances only retain an index into its
// fixed definition tables plus their own ECS state; they never mutate this
// class or copy its clips/skeleton.
class AnimGraph final
{
public:
    const AnimationResourceSet& resources() const { return *m_resources; }
    const std::shared_ptr<const AnimationResourceSet>& resourceSet() const { return m_resources; }
    const std::string& resourceSignature() const { return m_definition.resource_signature; }
    const std::vector<AnimGraphParameterDefinition>& parameters() const { return m_definition.parameters; }
    const std::vector<AnimGraphStateDefinition>& states() const { return m_definition.states; }
    const std::vector<AnimGraphMachineDefinition>& machines() const { return m_definition.machines; }
    const std::vector<AnimGraphTransitionDefinition>& transitions() const { return m_definition.transitions; }
    const std::vector<AnimGraphNotifyDefinition>& notifies() const { return m_definition.notifies; }

    int parameterIndex(std::string_view name) const;
    int machineIndex(std::string_view name) const;

private:
    friend class AnimGraphValidation::GraphBuilder;
    AnimGraph(AnimGraphDefinition definition, std::shared_ptr<const AnimationResourceSet> resources)
        : m_definition(std::move(definition)), m_resources(std::move(resources)) {}

    AnimGraphDefinition m_definition;
    std::shared_ptr<const AnimationResourceSet> m_resources;
};

namespace AnimGraphValidation
{
// Reports every structural issue in one call. A successful result is fully
// immutable and owner-bound to resources; callers publish only that result.
ENGINE_API std::shared_ptr<const AnimGraph> makeGraph(
    AnimGraphDefinition definition,
    std::shared_ptr<const AnimationResourceSet> resources,
    AnimGraphValidationReport& report);
}
