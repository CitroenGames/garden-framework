#include "LocalPlayerInputEcsBridge.hpp"

#include "Components/LocalPlayerInputComponent.hpp"
#include "InputActionSystem.hpp"
#include "world.hpp"

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

void LocalPlayerInputEcsBridge::update(world& game_world,
                             const InputActionSystem& input_actions)
{
    // Private ECS bridge implementing the Garden input system boundary.
    auto view = game_world.registry.view<LocalPlayerInputComponent>();
    for (entt::entity entity : view)
    {
        LocalPlayerInputComponent& local = view.get<LocalPlayerInputComponent>(entity);
        const bool pending_jump = local.jump_press_pending;
        local.clearIntent();
        if (!local.input_enabled)
            continue;

        local.move = actionAxis(input_actions, local.actions.move);
        local.jump_held = actionState(input_actions, local.actions.jump).held;
        local.jump_press_pending = pending_jump ||
            actionState(input_actions, local.actions.jump).press_count != 0;
        local.sprint_held = actionState(input_actions, local.actions.sprint).held;
        local.crouch_held = actionState(input_actions, local.actions.crouch).held;
        local.look = actionAxis(input_actions, local.actions.look);

        const float freecam_vertical = actionAxis(input_actions, local.actions.freecam_vertical).x;
        local.freecam_move = glm::vec3(-local.move.x, freecam_vertical, local.move.y);
        local.freecam_fast = actionState(input_actions, local.actions.freecam_fast).held;
    }
}

void LocalPlayerInputEcsBridge::setOwner(world& game_world,
                              EntityRef& current_owner,
                              EntityRef new_owner,
                              const LocalPlayerInputActionNames& actions)
{
    // Private ECS bridge for component ownership and structural mutation.
    auto& registry = game_world.registry;
    const entt::entity previous = resolveEntityRef(registry, current_owner);
    if (current_owner != new_owner ||
        (!current_owner.isNull() && previous == entt::null))
    {
        if (previous != entt::null &&
            registry.all_of<LocalPlayerInputComponent>(previous))
        {
            registry.remove<LocalPlayerInputComponent>(previous);
        }
        current_owner = {};
    }

    const entt::entity next = resolveEntityRef(registry, new_owner);
    if (next == entt::null)
        return;

    auto& local = registry.get_or_emplace<LocalPlayerInputComponent>(next);
    local.actions = actions;
    current_owner = new_owner;
}
