#include "LocalPlayerInputSystem.hpp"

#include "Components/LocalPlayerInputComponent.hpp"
#include "InputActionSystem.hpp"

namespace
{
    const ActionState& actionState(const InputActionSystem& actions, const std::string& name)
    {
        static const ActionState released{};
        return name.empty() ? released : actions.get(name.c_str());
    }

    glm::vec2 actionAxis(const InputActionSystem& actions, const std::string& name)
    {
        return name.empty() ? glm::vec2(0.0f) : actions.axis(name.c_str());
    }
}

void updateLocalPlayerInputs(entt::registry& registry,
                             const InputActionSystem& input_actions)
{
    auto view = registry.view<LocalPlayerInputComponent>();
    for (entt::entity entity : view)
    {
        LocalPlayerInputComponent& local = view.get<LocalPlayerInputComponent>(entity);
        local.clearIntent();
        if (!local.input_enabled)
            continue;

        local.move = actionAxis(input_actions, local.actions.move);
        local.jump_held = actionState(input_actions, local.actions.jump).held;
        local.sprint_held = actionState(input_actions, local.actions.sprint).held;
        local.crouch_held = actionState(input_actions, local.actions.crouch).held;
        local.look = actionAxis(input_actions, local.actions.look);

        const float freecam_vertical = actionAxis(input_actions, local.actions.freecam_vertical).x;
        local.freecam_move = glm::vec3(-local.move.x, freecam_vertical, local.move.y);
        local.freecam_fast = actionState(input_actions, local.actions.freecam_fast).held;
    }
}

void setLocalPlayerInputOwner(entt::registry& registry,
                              entt::entity& current_owner,
                              entt::entity new_owner,
                              const LocalPlayerInputActionNames& actions)
{
    if (current_owner != new_owner ||
        (current_owner != entt::null && !registry.valid(current_owner)))
    {
        if (registry.valid(current_owner) &&
            registry.all_of<LocalPlayerInputComponent>(current_owner))
        {
            registry.remove<LocalPlayerInputComponent>(current_owner);
        }
        current_owner = entt::null;
    }

    if (new_owner == entt::null || !registry.valid(new_owner))
        return;

    auto& local = registry.get_or_emplace<LocalPlayerInputComponent>(new_owner);
    local.actions = actions;
    current_owner = new_owner;
}
