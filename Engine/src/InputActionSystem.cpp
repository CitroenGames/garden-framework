#include "InputActionSystem.hpp"

#include "Utils/Log.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
    using Json = nlohmann::json;
    using Source = InputBinding::Source;

    constexpr float ActiveEpsilon = 0.0001f;
    constexpr float AxisButtonThreshold = 0.5f;

    const char* sourceName(Source source)
    {
        switch (source)
        {
        case Source::Key: return "key";
        case Source::MouseButton: return "mouse_button";
        case Source::MouseDeltaX: return "mouse_delta_x";
        case Source::MouseDeltaY: return "mouse_delta_y";
        case Source::GamepadButton: return "gamepad_button";
        case Source::GamepadAxis: return "gamepad_axis";
        }
        return "unknown";
    }

    bool parseSource(const std::string& value, Source& source)
    {
        if (value == "key") source = Source::Key;
        else if (value == "mouse_button") source = Source::MouseButton;
        else if (value == "mouse_delta_x") source = Source::MouseDeltaX;
        else if (value == "mouse_delta_y") source = Source::MouseDeltaY;
        else if (value == "gamepad_button") source = Source::GamepadButton;
        else if (value == "gamepad_axis") source = Source::GamepadAxis;
        else return false;
        return true;
    }

    bool readInt(const Json& value, int& output)
    {
        if (!value.is_number_integer())
            return false;

        if (value.is_number_unsigned())
        {
            const uint64_t number = value.get<uint64_t>();
            if (number > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                return false;
            output = static_cast<int>(number);
            return true;
        }

        const int64_t number = value.get<int64_t>();
        if (number < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
            number > static_cast<int64_t>(std::numeric_limits<int>::max()))
            return false;
        output = static_cast<int>(number);
        return true;
    }

    bool sameSource(const InputBinding& left, const InputBinding& right)
    {
        return left.source == right.source && left.code == right.code && left.slot == right.slot;
    }

    void incrementCount(uint32_t& count)
    {
        if (count < std::numeric_limits<uint32_t>::max())
            ++count;
    }
}

std::size_t InputActionSystem::SourceIdentityHash::operator()(const SourceIdentity& source) const
{
    const std::size_t source_hash = std::hash<uint8_t>{}(static_cast<uint8_t>(source.source));
    const std::size_t code_hash = std::hash<int>{}(source.code);
    const std::size_t combined = source_hash ^ (code_hash + 0x9e3779b9U + (source_hash << 6U) + (source_hash >> 2U));
    const std::size_t slot_hash = std::hash<int>{}(source.slot);
    return combined ^ (slot_hash + 0x9e3779b9U + (combined << 6U) + (combined >> 2U));
}

bool InputActionSystem::isValidActionType(ActionValueType type)
{
    switch (type)
    {
    case ActionValueType::Button:
    case ActionValueType::Axis1D:
    case ActionValueType::Axis2D:
        return true;
    }
    return false;
}

bool InputActionSystem::isReservedKey(int code)
{
    return code == SDL_SCANCODE_ESCAPE || code == SDL_SCANCODE_GRAVE;
}

InputActionSystem::SourceIdentity InputActionSystem::identityOf(const InputBinding& binding)
{
    return { binding.source, binding.code, binding.slot };
}

InputActionSystem::SourceIdentity InputActionSystem::identityOf(const RawInputTransition& transition)
{
    switch (transition.device)
    {
    case RawInputDevice::Keyboard: return { Source::Key, transition.code, 0 };
    case RawInputDevice::Mouse: return { Source::MouseButton, transition.code, 0 };
    case RawInputDevice::GamepadButton: return { Source::GamepadButton, transition.code, transition.slot };
    case RawInputDevice::GamepadAxis: return { Source::GamepadAxis, transition.code, transition.slot };
    }
    return {};
}

bool InputActionSystem::registerAction(const InputActionDef& action)
{
    if (action.name.empty() || !isValidActionType(action.type))
        return false;

    actions[action.name] = action;
    states.try_emplace(action.name, ActionState{});
    return true;
}

bool InputActionSystem::isValidBinding(const InputBinding& binding) const
{
    if (binding.action.empty() || actions.find(binding.action) == actions.end())
        return false;
    if (!std::isfinite(binding.scale) || binding.axis_target > 1)
        return false;

    switch (binding.source)
    {
    case Source::Key:
        return binding.slot == 0 && binding.code > SDL_SCANCODE_UNKNOWN && binding.code < SDL_SCANCODE_COUNT &&
            !isReservedKey(binding.code);
    case Source::MouseButton:
        return binding.slot == 0 && binding.code > 0 && binding.code < 256;
    case Source::MouseDeltaX:
    case Source::MouseDeltaY:
        return binding.code == 0 && binding.slot == 0;
    case Source::GamepadButton:
        return binding.slot >= 0 && static_cast<std::size_t>(binding.slot) < InputGamepadSlotCount &&
            binding.code >= 0 && binding.code < SDL_GAMEPAD_BUTTON_COUNT;
    case Source::GamepadAxis:
        return binding.slot >= 0 && static_cast<std::size_t>(binding.slot) < InputGamepadSlotCount &&
            binding.code >= 0 && binding.code < SDL_GAMEPAD_AXIS_COUNT;
    }
    return false;
}

bool InputActionSystem::registerContext(const MappingContext& context)
{
    if (context.name.empty())
        return false;
    for (const InputBinding& binding : context.bindings)
    {
        if (!isValidBinding(binding))
            return false;
    }

    default_contexts[context.name] = context;
    rebuildContext(context.name);
    return true;
}

void InputActionSystem::rebuildContext(const std::string& name)
{
    MappingContext effective;
    const auto defaults = default_contexts.find(name);
    if (defaults != default_contexts.end())
        effective = defaults->second;
    else
        effective.name = name;

    const auto overrides = context_overrides.find(name);
    if (overrides != context_overrides.end())
    {
        for (const InputBinding& binding : overrides->second)
        {
            effective.bindings.erase(
                std::remove_if(effective.bindings.begin(), effective.bindings.end(), [&](const InputBinding& existing) {
                    return sameSource(existing, binding);
                }),
                effective.bindings.end());
            effective.bindings.push_back(binding);
        }
    }
    contexts[name] = std::move(effective);
}

bool InputActionSystem::pushContext(const MappingContext& context)
{
    if (!registerContext(context))
        return false;
    return pushContext(context.name.c_str());
}

bool InputActionSystem::pushContext(const char* name)
{
    if (!name || !*name || contexts.find(name) == contexts.end())
        return false;

    active_contexts.erase(
        std::remove(active_contexts.begin(), active_contexts.end(), name),
        active_contexts.end());
    active_contexts.emplace_back(name);
    return true;
}

void InputActionSystem::popContext(const char* name)
{
    if (!name)
        return;
    active_contexts.erase(
        std::remove(active_contexts.begin(), active_contexts.end(), name),
        active_contexts.end());
}

std::vector<const MappingContext*> InputActionSystem::orderedActiveContexts() const
{
    std::vector<const MappingContext*> result;
    result.reserve(active_contexts.size());
    for (auto it = active_contexts.rbegin(); it != active_contexts.rend(); ++it)
    {
        const auto found = contexts.find(*it);
        if (found != contexts.end())
            result.push_back(&found->second);
    }

    std::stable_sort(result.begin(), result.end(), [](const MappingContext* left, const MappingContext* right) {
        return left->priority > right->priority;
    });
    return result;
}

InputActionSystem::DigitalState InputActionSystem::makeFrameStartState(const InputManager& input) const
{
    DigitalState state;
    for (const MappingContext* context : orderedActiveContexts())
    {
        for (const InputBinding& binding : context->bindings)
        {
            if (binding.source == Source::Key)
            {
                const auto id = identityOf(binding);
                state[id] = input.is_key_held(static_cast<SDL_Scancode>(binding.code)) ? 1.0f : 0.0f;
            }
            else if (binding.source == Source::MouseButton)
            {
                const auto id = identityOf(binding);
                state[id] = input.is_mouse_button_held(static_cast<uint8_t>(binding.code)) ? 1.0f : 0.0f;
            }
            else if (binding.source == Source::GamepadButton)
            {
                const auto id = identityOf(binding);
                const uint32_t mask = uint32_t{ 1 } << static_cast<uint32_t>(binding.code);
                state[id] = (input.gamepad(binding.slot).buttons_held & mask) != 0 ? 1.0f : 0.0f;
            }
            else if (binding.source == Source::GamepadAxis)
            {
                const auto id = identityOf(binding);
                state[id] = input.gamepad(binding.slot).axes[static_cast<std::size_t>(binding.code)];
            }
        }
    }

    const auto& transitions = input.get_button_transitions();
    for (auto it = transitions.rbegin(); it != transitions.rend(); ++it)
    {
        const SourceIdentity id = identityOf(*it);
        const auto found = state.find(id);
        if (found != state.end())
            found->second = it->previous_value;
    }
    return state;
}

InputActionSystem::EvaluatedActions InputActionSystem::evaluateSources(
    const DigitalState& digital_state, float mouse_delta_x, float mouse_delta_y) const
{
    EvaluatedActions output;
    for (const auto& [name, action] : actions)
    {
        (void)action;
        output.emplace(name, EvaluatedAction{});
    }

    std::unordered_set<SourceIdentity, SourceIdentityHash> consumed_sources;
    for (const MappingContext* context : orderedActiveContexts())
    {
        std::unordered_set<SourceIdentity, SourceIdentityHash> consumed_after_context;
        for (const InputBinding& binding : context->bindings)
        {
            const auto action_it = actions.find(binding.action);
            auto output_it = output.find(binding.action);
            if (action_it == actions.end() || output_it == output.end())
                continue;

            const SourceIdentity id = identityOf(binding);
            if (consumed_sources.find(id) != consumed_sources.end())
                continue;

            float raw_value = 0.0f;
            if (binding.source == Source::Key || binding.source == Source::MouseButton ||
                binding.source == Source::GamepadButton || binding.source == Source::GamepadAxis)
            {
                const auto state_it = digital_state.find(id);
                if (state_it != digital_state.end() && state_it->second)
                    raw_value = state_it->second;
            }
            else if (binding.source == Source::MouseDeltaX)
                raw_value = mouse_delta_x;
            else if (binding.source == Source::MouseDeltaY)
                raw_value = mouse_delta_y;

            EvaluatedAction& action_output = output_it->second;
            float contribution = raw_value * binding.scale;
            if (binding.invert)
                contribution = -contribution;

            if (action_it->second.type == ActionValueType::Button)
            {
                const bool axis_source = binding.source == Source::GamepadAxis;
                const bool active = axis_source
                    ? contribution >= AxisButtonThreshold
                    : std::abs(raw_value) > ActiveEpsilon;
                if (active)
                    action_output.value.x = 1.0f;
                if (action_it->second.consume && active)
                    consumed_after_context.insert(id);
            }
            else
            {
                if (std::abs(contribution) <= ActiveEpsilon)
                    continue;

                if (action_it->second.type == ActionValueType::Axis1D)
                    action_output.value.x += contribution;
                else if (binding.axis_target == 0)
                    action_output.value.x += contribution;
                else
                    action_output.value.y += contribution;
            }

            if (action_it->second.type != ActionValueType::Button && action_it->second.consume)
                consumed_after_context.insert(id);
        }

        consumed_sources.insert(consumed_after_context.begin(), consumed_after_context.end());
        if (context->modal)
            break;
    }

    for (auto& [name, value] : output)
    {
        const auto action_it = actions.find(name);
        if (action_it == actions.end())
            continue;

        if (action_it->second.type == ActionValueType::Button)
        {
            value.held = value.value.x > 0.5f;
            value.value = glm::vec2(value.held ? 1.0f : 0.0f, 0.0f);
        }
        else if (action_it->second.type == ActionValueType::Axis1D)
        {
            value.value.x = glm::clamp(value.value.x, -1.0f, 1.0f);
            value.value.y = 0.0f;
            value.held = std::abs(value.value.x) > ActiveEpsilon;
        }
        else
        {
            const float length = glm::length(value.value);
            if (length > 1.0f)
                value.value /= length;
            value.held = glm::length(value.value) > ActiveEpsilon;
        }
    }
    return output;
}

InputActionSystem::EvaluatedActions InputActionSystem::evaluateDigital(const DigitalState& state) const
{
    return evaluateSources(state, 0.0f, 0.0f);
}

InputActionSystem::EvaluatedActions InputActionSystem::evaluateCurrent(
    const InputManager& input, const DigitalState& state) const
{
    return evaluateSources(state, input.get_mouse_delta_x(), input.get_mouse_delta_y());
}

void InputActionSystem::evaluate(const InputManager& input, float dt)
{
    (void)dt;
    const auto old_states = states;
    const auto transitions = input.get_button_transitions();
    DigitalState digital_state = makeFrameStartState(input);
    EvaluatedActions current = evaluateDigital(digital_state);

    std::unordered_map<std::string, ActionState> next_states;
    next_states.reserve(actions.size());
    for (const auto& [name, action] : actions)
    {
        (void)action;
        ActionState next{};
        const auto found = current.find(name);
        next.held = found != current.end() && found->second.held;
        next_states.emplace(name, next);
    }

    for (const RawInputTransition& transition : transitions)
    {
        const SourceIdentity id = identityOf(transition);
        digital_state[id] = transition.value;
        current = evaluateDigital(digital_state);

        for (auto& [name, next] : next_states)
        {
            const auto evaluated = current.find(name);
            const bool new_held = evaluated != current.end() && evaluated->second.held;
            if (new_held == next.held)
                continue;

            next.held = new_held;
            if (new_held)
            {
                next.pressed = true;
                incrementCount(next.press_count);
            }
            else
            {
                next.released = true;
                incrementCount(next.release_count);
            }
        }
    }

    const EvaluatedActions final_actions = evaluateCurrent(input, digital_state);
    for (auto& [name, next] : next_states)
    {
        const auto evaluated = final_actions.find(name);
        if (evaluated != final_actions.end())
        {
            next.value = evaluated->second.value;
            next.held = evaluated->second.held;
        }
        next_states[name] = next;
    }
    states = std::move(next_states);

    std::vector<uint64_t> callback_ids;
    callback_ids.reserve(callbacks.size());
    for (const auto& [id, callback] : callbacks)
    {
        (void)callback;
        callback_ids.push_back(id);
    }
    std::sort(callback_ids.begin(), callback_ids.end());

    for (const uint64_t id : callback_ids)
    {
        const auto callback_it = callbacks.find(id);
        if (callback_it == callbacks.end())
            continue;
        const auto old_it = old_states.find(callback_it->second.action);
        const auto new_it = states.find(callback_it->second.action);
        if (new_it == states.end())
            continue;

        bool changed = new_it->second.pressed || new_it->second.released;
        if (old_it == old_states.end())
            changed = true;
        else
        {
            changed = changed || old_it->second.held != new_it->second.held ||
                old_it->second.value.x != new_it->second.value.x ||
                old_it->second.value.y != new_it->second.value.y;
        }
        if (changed && callback_it->second.callback)
        {
            const Callback callback = callback_it->second.callback;
            const ActionState callback_state = new_it->second;
            callback(callback_state);
        }
    }
}

const ActionState& InputActionSystem::get(const char* name) const
{
    static const ActionState Released{};
    if (!name)
        return Released;
    const auto found = states.find(name);
    return found != states.end() ? found->second : Released;
}

bool InputActionSystem::wasPressed(const char* name) const
{
    return get(name).pressed;
}

glm::vec2 InputActionSystem::axis(const char* name) const
{
    return get(name).value;
}

uint64_t InputActionSystem::bindCallback(const char* name, Callback callback)
{
    if (!name || !*name || !callback || actions.find(name) == actions.end())
        return 0;

    const uint64_t id = next_callback_id++;
    callbacks.emplace(id, CallbackBinding{ name, std::move(callback) });
    return id;
}

bool InputActionSystem::unbindCallback(uint64_t id)
{
    return callbacks.erase(id) != 0;
}

bool InputActionSystem::rebind(const char* context_name, const char* action_name, InputBinding binding)
{
    if (!context_name || !action_name || !*context_name || !*action_name)
        return false;
    if (contexts.find(context_name) == contexts.end() || actions.find(action_name) == actions.end())
        return false;

    binding.action = action_name;
    if (!isValidBinding(binding))
        return false;

    auto& overrides = context_overrides[context_name];
    auto it = overrides.begin();
    while (it != overrides.end())
    {
        if (!sameSource(*it, binding))
        {
            ++it;
            continue;
        }

        // Overrides are source-scoped. Replacing the same source also clears
        // any prior action that claimed it, while other bindings remain intact.
        it = overrides.erase(it);
    }

    overrides.push_back(std::move(binding));
    rebuildContext(context_name);
    return true;
}

std::string InputActionSystem::serialiseProfile() const
{
    Json root;
    root["version"] = 1;
    root["contexts"] = Json::array();

    std::vector<std::string> context_names;
    context_names.reserve(context_overrides.size());
    for (const auto& [name, overrides] : context_overrides)
    {
        if (!overrides.empty())
            context_names.push_back(name);
    }
    std::sort(context_names.begin(), context_names.end());

    for (const std::string& name : context_names)
    {
        Json context_json;
        context_json["name"] = name;
        context_json["bindings"] = Json::array();
        for (const InputBinding& binding : context_overrides.at(name))
        {
            Json binding_json;
            binding_json["action"] = binding.action;
            binding_json["source"] = sourceName(binding.source);
            binding_json["code"] = binding.code;
            binding_json["slot"] = binding.slot;
            binding_json["scale"] = binding.scale;
            binding_json["invert"] = binding.invert;
            binding_json["axis_target"] = binding.axis_target;
            context_json["bindings"].push_back(std::move(binding_json));
        }
        root["contexts"].push_back(std::move(context_json));
    }
    return root.dump(2);
}

bool InputActionSystem::loadProfile(const std::string& profile, std::size_t* warning_count)
{
    last_profile_warning_count = 0;
    if (warning_count)
        *warning_count = 0;

    const Json root = Json::parse(profile, nullptr, false);
    int version = 0;
    if (root.is_discarded() || !root.is_object() || !root.contains("version") ||
        !readInt(root["version"], version) || version != 1 ||
        !root.contains("contexts") || !root["contexts"].is_array())
        return false;

    std::unordered_map<std::string, std::vector<InputBinding>> loaded_contexts;
    for (const Json& context_json : root["contexts"])
    {
        if (!context_json.is_object() || !context_json.contains("name") || !context_json["name"].is_string() ||
            !context_json.contains("bindings") || !context_json["bindings"].is_array())
        {
            ++last_profile_warning_count;
            continue;
        }

        const std::string context_name = context_json["name"].get<std::string>();
        if (context_name.empty())
        {
            ++last_profile_warning_count;
            continue;
        }

        if (loaded_contexts.find(context_name) != loaded_contexts.end())
        {
            ++last_profile_warning_count;
            continue;
        }

        std::vector<InputBinding> valid_bindings;
        for (const Json& binding_json : context_json["bindings"])
        {
            if (!binding_json.is_object() || !binding_json.contains("action") || !binding_json["action"].is_string() ||
                !binding_json.contains("source") || !binding_json["source"].is_string())
            {
                ++last_profile_warning_count;
                continue;
            }

            InputBinding binding;
            binding.action = binding_json["action"].get<std::string>();
            if (actions.find(binding.action) == actions.end())
            {
                ++last_profile_warning_count;
                continue;
            }
            if (!parseSource(binding_json["source"].get<std::string>(), binding.source))
            {
                ++last_profile_warning_count;
                continue;
            }

            bool valid_fields = true;
            if (binding_json.contains("slot") && !readInt(binding_json["slot"], binding.slot))
                valid_fields = false;
            if (binding_json.contains("code"))
            {
                if (!readInt(binding_json["code"], binding.code))
                    valid_fields = false;
            }
            if (binding_json.contains("scale"))
            {
                if (!binding_json["scale"].is_number())
                    valid_fields = false;
                else
                    binding.scale = binding_json["scale"].get<float>();
            }
            if (binding_json.contains("invert"))
            {
                if (!binding_json["invert"].is_boolean())
                    valid_fields = false;
                else
                    binding.invert = binding_json["invert"].get<bool>();
            }
            if (binding_json.contains("axis_target"))
            {
                int axis_target = 0;
                if (!readInt(binding_json["axis_target"], axis_target))
                    valid_fields = false;
                else
                {
                    if (axis_target < 0 || axis_target > 1)
                        valid_fields = false;
                    else
                        binding.axis_target = static_cast<uint8_t>(axis_target);
                }
            }
            if (!valid_fields || !isValidBinding(binding))
            {
                ++last_profile_warning_count;
                continue;
            }
            valid_bindings.push_back(std::move(binding));
        }

        loaded_contexts.emplace(context_name, std::move(valid_bindings));
    }

    context_overrides.clear();
    std::unordered_set<std::string> contexts_to_rebuild;
    for (const auto& [name, context] : default_contexts)
    {
        (void)context;
        contexts_to_rebuild.insert(name);
    }
    for (const auto& [name, context] : contexts)
    {
        (void)context;
        contexts_to_rebuild.insert(name);
    }
    for (const auto& [name, overrides] : loaded_contexts)
    {
        context_overrides.emplace(name, overrides);
        contexts_to_rebuild.insert(name);
    }
    for (const std::string& name : contexts_to_rebuild)
        rebuildContext(name);

    if (warning_count)
        *warning_count = last_profile_warning_count;
    if (last_profile_warning_count > 0)
        LOG_ENGINE_WARN("Input profile skipped {} unknown or invalid entries", last_profile_warning_count);
    return true;
}
