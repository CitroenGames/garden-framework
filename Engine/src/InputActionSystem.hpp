#pragma once

#include "EngineExport.h"
#include "InputManager.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

enum class ActionValueType : uint8_t
{
    Button,
    Axis1D,
    Axis2D
};

struct InputActionDef
{
    std::string name;
    ActionValueType type = ActionValueType::Button;
    bool consume = false;
};

struct ActionState
{
    bool held = false;
    bool pressed = false;
    bool released = false;
    glm::vec2 value{ 0.0f };
    uint32_t press_count = 0;
    uint32_t release_count = 0;
};

struct InputBinding
{
    enum class Source : uint8_t
    {
        Key,
        MouseButton,
        MouseDeltaX,
        MouseDeltaY,
        GamepadButton,
        GamepadAxis
    };

    std::string action;
    Source source = Source::Key;
    int code = 0; // SDL enum/scancode/button id; delta sources use zero.
    float scale = 1.0f;
    bool invert = false;
    uint8_t axis_target = 0; // Axis2D: 0 = x, 1 = y.
    int slot = 0; // Logical gamepad slot; zero for keyboard/mouse.
};

struct MappingContext
{
    std::string name;
    int priority = 0;
    bool modal = false;
    std::vector<InputBinding> bindings;
};

// Runtime action mapping layer over InputManager's keyboard, mouse, and
// slot-aware gamepad state. Action definitions and mapping contexts remain
// regular game data; this class owns evaluation and callback lifetime.
class ENGINE_API InputActionSystem
{
public:
    using Callback = std::function<void(const ActionState&)>;

    bool registerAction(const InputActionDef& action);
    bool registerContext(const MappingContext& context);
    bool pushContext(const MappingContext& context);
    bool pushContext(const char* name);
    void popContext(const char* name);

    void evaluate(const InputManager& input, float dt = 0.0f);

    const ActionState& get(const char* name) const;
    bool wasPressed(const char* name) const;
    glm::vec2 axis(const char* name) const;

    uint64_t bindCallback(const char* name, Callback callback);
    bool unbindCallback(uint64_t id);

    // Rebinding adds or updates the incoming source. Other bindings for the
    // action remain. A same-context source conflict is cleared first.
    bool rebind(const char* context, const char* action, InputBinding binding);

    std::string serialiseProfile() const;
    bool loadProfile(const std::string& json, std::size_t* warning_count = nullptr);
    std::size_t lastProfileWarningCount() const { return last_profile_warning_count; }

private:
    struct SourceIdentity
    {
        InputBinding::Source source = InputBinding::Source::Key;
        int code = 0;
        int slot = 0;

        bool operator==(const SourceIdentity& other) const
        {
            return source == other.source && code == other.code && slot == other.slot;
        }
    };

    struct SourceIdentityHash
    {
        std::size_t operator()(const SourceIdentity& source) const;
    };

    struct CallbackBinding
    {
        std::string action;
        Callback callback;
    };

    struct EvaluatedAction
    {
        glm::vec2 value{ 0.0f };
        bool held = false;
    };

    using DigitalState = std::unordered_map<SourceIdentity, float, SourceIdentityHash>;
    using EvaluatedActions = std::unordered_map<std::string, EvaluatedAction>;

    std::unordered_map<std::string, InputActionDef> actions;
    std::unordered_map<std::string, ActionState> states;
    std::unordered_map<std::string, MappingContext> default_contexts;
    std::unordered_map<std::string, std::vector<InputBinding>> context_overrides;
    std::unordered_map<std::string, MappingContext> contexts;
    std::vector<std::string> active_contexts;
    std::unordered_map<uint64_t, CallbackBinding> callbacks;
    uint64_t next_callback_id = 1;
    std::size_t last_profile_warning_count = 0;

    bool isValidBinding(const InputBinding& binding) const;
    static bool isValidActionType(ActionValueType type);
    static bool isReservedKey(int code);
    static SourceIdentity identityOf(const InputBinding& binding);
    static SourceIdentity identityOf(const RawInputTransition& transition);
    void rebuildContext(const std::string& name);
    DigitalState makeFrameStartState(const InputManager& input) const;
    EvaluatedActions evaluateDigital(const DigitalState& state) const;
    EvaluatedActions evaluateCurrent(const InputManager& input, const DigitalState& state) const;
    EvaluatedActions evaluateSources(const DigitalState& state, float mouse_delta_x, float mouse_delta_y) const;
    std::vector<const MappingContext*> orderedActiveContexts() const;
};
