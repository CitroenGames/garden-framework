#include "Gameplay/GameplaySystems.hpp"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"

#include <cmath>

DamageResult DamageSystem::applyDamage(entt::registry& registry, entt::entity target,
                                       const DamageInfo& info)
{
    return applyDamage(registry, target, info, 0);
}

DamageResult DamageSystem::applyDamage(entt::registry& registry, entt::entity target,
                                       const DamageInfo& info, uint64_t current_tick)
{
    DamageResult result;
    if (target == entt::null || !registry.valid(target))
    {
        result.blocked = true;
        return result;
    }
    auto* health = registry.try_get<HealthComponent>(target);
    if (!health)
    {
        result.blocked = true;
        return result;
    }
    result.remaining_health = health->current;
    if (!health->alive)
    {
        result.blocked = true;
        return result;
    }
    if (!std::isfinite(info.amount) || info.amount <= 0.0f)
    {
        result.blocked = true;
        return result;
    }
    if (health->isInvulnerable(current_tick))
    {
        result.blocked = true;
        return result;
    }

    const float before = health->current;
    float after = before - info.amount;
    if (after < 0.0f)
        after = 0.0f;
    health->current = after;
    const bool killed = (after <= 0.0f && before > 0.0f);
    if (killed)
        health->alive = false;

    result.applied = true;
    result.killed = killed;
    result.applied_amount = before - after;
    result.remaining_health = after;

    DamageAppliedEvent applied;
    applied.target = entityRefOf(registry, target);
    applied.instigator = info.instigator;
    applied.source = info.source;
    applied.amount = result.applied_amount;
    applied.remaining_health = after;
    applied.damage_type = info.damage_type;
    applied.killed = killed;
    applied.tick = current_tick;
    EventBus::get().publish(applied);

    if (killed)
    {
        EntityDiedEvent died;
        died.target = applied.target;
        died.instigator = info.instigator;
        died.source = info.source;
        died.damage_type = info.damage_type;
        died.tick = current_tick;
        EventBus::get().publish(died);
    }
    return result;
}

namespace Gameplay
{
size_t countTeamMembers(const entt::registry& registry, uint8_t team_id)
{
    size_t n = 0;
    auto view = registry.view<TeamComponent>();
    for (entt::entity e : view)
        if (view.get<TeamComponent>(e).team_id == team_id)
            ++n;
    return n;
}

std::vector<entt::entity> getTeamMembers(const entt::registry& registry, uint8_t team_id)
{
    std::vector<entt::entity> out;
    auto view = registry.view<TeamComponent>();
    for (entt::entity e : view)
        if (view.get<TeamComponent>(e).team_id == team_id)
            out.push_back(e);
    return out;
}

bool isOnTeam(const entt::registry& registry, entt::entity e, uint8_t team_id)
{
    if (e == entt::null || !registry.valid(e))
        return false;
    const auto* t = registry.try_get<TeamComponent>(e);
    return t && t->team_id == team_id;
}

bool areTeammates(const entt::registry& registry, entt::entity a, entt::entity b)
{
    if (a == entt::null || b == entt::null || !registry.valid(a) || !registry.valid(b))
        return false;
    const auto* ta = registry.try_get<TeamComponent>(a);
    const auto* tb = registry.try_get<TeamComponent>(b);
    return ta && tb && ta->team_id == tb->team_id;
}

bool startChannel(entt::registry& registry, entt::entity e, uint32_t channel_id, float duration)
{
    if (e == entt::null || !registry.valid(e))
        return false;
    auto* ch = registry.try_get<ChannelComponent>(e);
    if (!ch)
        return false;
    ch->start(channel_id, duration);
    return ch->active;
}

bool cancelChannel(entt::registry& registry, entt::entity e, uint64_t current_tick)
{
    if (e == entt::null || !registry.valid(e))
        return false;
    auto* ch = registry.try_get<ChannelComponent>(e);
    if (!ch || !ch->active)
        return false;
    const uint32_t id = ch->channel_id;
    ch->cancel();
    ChannelCancelledEvent ev;
    ev.entity = entityRefOf(registry, e);
    ev.channel_id = id;
    ev.tick = current_tick;
    EventBus::get().publish(ev);
    return true;
}
} // namespace Gameplay

void tickCountdownTimers(entt::registry& registry, const Tick::SimulationTickContext& ctx)
{
    auto view = registry.view<CountdownTimerComponent>();
    // Collect expiries first: publishing handlers may mutate the registry.
    struct Expiry { uint32_t timer_id; bool repeating; };
    std::vector<Expiry> expired;
    for (entt::entity e : view)
    {
        auto& timer = view.get<CountdownTimerComponent>(e);
        if (!timer.running)
            continue;
        if (timer.tick(ctx.fixed_delta))
        {
            Expiry ex;
            ex.timer_id = static_cast<uint32_t>(entt::to_integral(e));
            ex.repeating = timer.loop;
            expired.push_back(ex);
        }
    }
    for (const auto& ex : expired)
        EventBus::get().queue(TimerExpiredEvent{ex.timer_id, ex.repeating});
}

void tickChannels(entt::registry& registry, const Tick::SimulationTickContext& ctx)
{
    auto view = registry.view<ChannelComponent>();
    struct Done { EntityRef ref; uint32_t id; };
    std::vector<Done> done;
    for (entt::entity e : view)
    {
        auto& ch = view.get<ChannelComponent>(e);
        if (!ch.active)
            continue;
        if (ch.tick(ctx.fixed_delta))
            done.push_back(Done{entityRefOf(registry, e), ch.channel_id});
    }
    for (const auto& d : done)
    {
        ChannelCompletedEvent ev;
        ev.entity = d.ref;
        ev.channel_id = d.id;
        ev.tick = ctx.tick_index;
        EventBus::get().publish(ev);
    }
}

void registerGameplaySystems(SystemScheduler& scheduler)
{
    {
        SystemDescriptor d;
        d.name = "engine.gameplay.countdown_timers";
        d.phase = Phase::Gameplay;
        d.update = [](entt::registry& r, const Tick::SimulationTickContext& ctx) {
            tickCountdownTimers(r, ctx);
        };
        scheduler.addSystem(std::move(d));
    }
    {
        SystemDescriptor d;
        d.name = "engine.gameplay.channels";
        d.phase = Phase::Gameplay;
        d.update = [](entt::registry& r, const Tick::SimulationTickContext& ctx) {
            tickChannels(r, ctx);
        };
        scheduler.addSystem(std::move(d));
    }
}

bool registerGameplayPrimitives(SystemScheduler& scheduler)
{
    const bool before = scheduler.hasSystem("engine.gameplay.countdown_timers");
    registerGameplaySystems(scheduler);
    return scheduler.hasSystem("engine.gameplay.countdown_timers") &&
           scheduler.hasSystem("engine.gameplay.channels") && !before;
}
