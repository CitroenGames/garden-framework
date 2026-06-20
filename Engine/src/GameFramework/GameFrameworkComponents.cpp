#include "GameFramework/GameFrameworkComponents.hpp"

namespace GameFramework
{
namespace
{
struct GameplayFrameworkEntityContext
{
    entt::entity game_mode = entt::null;
    entt::entity game_state = entt::null;
};

GameplayFrameworkEntityContext& getContext(entt::registry& registry)
{
    if (auto* context = registry.ctx().find<GameplayFrameworkEntityContext>())
        return *context;
    return registry.ctx().emplace<GameplayFrameworkEntityContext>();
}

const GameplayFrameworkEntityContext* findContext(const entt::registry& registry)
{
    return registry.ctx().find<GameplayFrameworkEntityContext>();
}

template<typename Component>
entt::entity findFirstEntityWith(const entt::registry& registry)
{
    auto view = registry.view<Component>();
    for (entt::entity entity : view)
        return entity;
    return entt::null;
}
}

entt::entity getGameModeEntity(const entt::registry& registry)
{
    const GameplayFrameworkEntityContext* context = findContext(registry);
    if (context && registry.valid(context->game_mode) &&
        registry.all_of<GameModeComponent>(context->game_mode))
        return context->game_mode;

    return findFirstEntityWith<GameModeComponent>(registry);
}

entt::entity getOrCreateGameModeEntity(entt::registry& registry)
{
    GameplayFrameworkEntityContext& context = getContext(registry);
    if (registry.valid(context.game_mode) &&
        registry.all_of<GameModeComponent>(context.game_mode))
        return context.game_mode;

    context.game_mode = findFirstEntityWith<GameModeComponent>(registry);
    if (context.game_mode != entt::null)
        return context.game_mode;

    context.game_mode = registry.create();
    registry.emplace<GameModeComponent>(context.game_mode);
    return context.game_mode;
}

entt::entity getGameStateEntity(const entt::registry& registry)
{
    const GameplayFrameworkEntityContext* context = findContext(registry);
    if (context && registry.valid(context->game_state) &&
        registry.all_of<GameStateComponent>(context->game_state))
        return context->game_state;

    return findFirstEntityWith<GameStateComponent>(registry);
}

entt::entity getOrCreateGameStateEntity(entt::registry& registry)
{
    GameplayFrameworkEntityContext& context = getContext(registry);
    if (registry.valid(context.game_state) &&
        registry.all_of<GameStateComponent>(context.game_state))
        return context.game_state;

    context.game_state = findFirstEntityWith<GameStateComponent>(registry);
    if (context.game_state != entt::null)
        return context.game_state;

    context.game_state = registry.create();
    registry.emplace<GameStateComponent>(context.game_state);
    return context.game_state;
}

entt::entity findPlayerStateEntity(const entt::registry& registry, uint16_t player_id)
{
    auto view = registry.view<PlayerStateComponent>();
    for (entt::entity entity : view)
    {
        const auto& player_state = view.get<PlayerStateComponent>(entity);
        if (player_state.player_id == player_id)
            return entity;
    }

    return entt::null;
}

PlayerStateComponent* getPlayerStateComponent(entt::registry& registry, uint16_t player_id)
{
    const entt::entity entity = findPlayerStateEntity(registry, player_id);
    return registry.valid(entity) ? registry.try_get<PlayerStateComponent>(entity) : nullptr;
}

const PlayerStateComponent* getPlayerStateComponent(const entt::registry& registry, uint16_t player_id)
{
    const entt::entity entity = findPlayerStateEntity(registry, player_id);
    return registry.valid(entity) ? registry.try_get<PlayerStateComponent>(entity) : nullptr;
}

void copyPlayerStateToComponent(PlayerStateComponent& component, const PlayerState& player_state)
{
    component.player_id = player_state.player_id;
    component.player_name = player_state.player_name;
    component.score = player_state.score;
    component.ping_ms = player_state.ping_ms;
    component.is_spectator = player_state.is_spectator;
    component.is_inactive = player_state.is_inactive;
    component.start_time = player_state.start_time;
    component.pawn = player_state.pawn;
}
}
