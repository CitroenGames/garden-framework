#pragma once

#include "EngineExport.h"
#include "Components/IdentityComponent.hpp"
#include "Reflection/Reflector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <entt/entt.hpp>

// Generic gameplay primitives (EngineCore). No product/game-specific
// vocabulary: callers decide what a unit, slot item, or channel means.

// ---------------------------------------------------------------------------
// Team
// ---------------------------------------------------------------------------
struct TeamComponent
{
    uint8_t team_id = 0;

    static void reflect(Reflector<TeamComponent>& r)
    {
        r.display("Team").category("Gameplay");
        r.field<&TeamComponent::team_id>("team_id")
            .tooltip("Opaque team identifier (game-defined)").category("Team");
    }
};

// ---------------------------------------------------------------------------
// Countdown timer (ECS, fixed-tick). Distinct from the wall-clock
// TimerSystem singleton: this ticks with SimulationTickContext::fixed_delta
// via the Gameplay-phase system and reuses the TimerExpiredEvent pattern.
// ---------------------------------------------------------------------------
struct CountdownTimerComponent
{
    float duration = 0.0f;
    float remaining = 0.0f;
    bool running = false;
    bool loop = false;

    void start(float dur, bool loop_ = false)
    {
        duration = dur > 0.0f && std::isfinite(dur) ? dur : 0.0f;
        remaining = duration;
        loop = loop_;
        running = duration > 0.0f;
    }
    void stop() { running = false; }
    void reset()
    {
        remaining = duration;
        running = duration > 0.0f;
    }
    // Returns true exactly on the tick the timer expires.
    bool tick(float dt)
    {
        if (!running || !(dt > 0.0f) || !std::isfinite(dt))
            return false;
        remaining -= dt;
        if (remaining > 1e-6f)
            return false;
        if (loop && duration > 0.0f)
        {
            // Carry the (tiny negative) remainder to stay on-grid.
            remaining = duration + remaining;
            if (remaining <= 1e-6f)
                remaining = duration;
            if (remaining > duration)
                remaining = duration;
        }
        else
        {
            remaining = 0.0f;
            running = false;
        }
        return true;
    }

    static void reflect(Reflector<CountdownTimerComponent>& r)
    {
        r.display("Countdown Timer").category("Gameplay");
        r.field<&CountdownTimerComponent::duration>("duration")
            .tooltip("Total duration in seconds").drag(0.01f).range(0.0f, 100000.0f).category("Timer");
        r.field<&CountdownTimerComponent::remaining>("remaining")
            .visible().category("Timer");
        r.field<&CountdownTimerComponent::running>("running").category("Timer");
        r.field<&CountdownTimerComponent::loop>("loop").category("Timer");
    }
};

// ---------------------------------------------------------------------------
// Resource wallet. The game decides what the balance means.
// ---------------------------------------------------------------------------
struct ResourceWalletComponent
{
    int64_t balance = 0;
    int64_t cap = std::numeric_limits<int64_t>::max();

    bool canAfford(int64_t amount) const
    {
        return amount >= 0 && balance >= amount;
    }
    // No mutation on failure (insufficient funds or negative amount).
    bool trySpend(int64_t amount)
    {
        if (amount < 0 || balance < amount)
            return false;
        balance -= amount;
        return true;
    }
    // Clamped to [0, cap]. Returns the amount actually added.
    int64_t add(int64_t amount)
    {
        if (amount <= 0)
            return 0;
        const int64_t effective_cap = cap < 0 ? std::numeric_limits<int64_t>::max() : cap;
        if (balance >= effective_cap)
            return 0;
        const int64_t room = effective_cap - balance;
        const int64_t added = std::min(amount, room);
        balance += added;
        return added;
    }
    // Clamped removal; returns amount actually removed.
    int64_t remove(int64_t amount)
    {
        if (amount <= 0 || balance <= 0)
            return 0;
        const int64_t removed = std::min(amount, balance);
        balance -= removed;
        return removed;
    }
    void setBalance(int64_t value)
    {
        const int64_t effective_cap = cap < 0 ? std::numeric_limits<int64_t>::max() : cap;
        balance = std::clamp<int64_t>(value, 0, effective_cap);
    }
    void setCap(int64_t new_cap)
    {
        cap = new_cap < 0 ? std::numeric_limits<int64_t>::max() : new_cap;
        if (balance > cap)
            balance = cap;
        if (balance < 0)
            balance = 0;
    }

    static void reflect(Reflector<ResourceWalletComponent>& r)
    {
        r.display("Resource Wallet").category("Gameplay");
        r.field<&ResourceWalletComponent::balance>("balance")
            .tooltip("Current balance (game-defined unit)").category("Wallet");
        r.field<&ResourceWalletComponent::cap>("cap")
            .tooltip("Maximum balance (clamp)").category("Wallet");
    }
};

// ---------------------------------------------------------------------------
// Inventory: fixed slots of {item_def_id, count} plus an active slot.
// No knowledge of what an item definition is.
// ---------------------------------------------------------------------------
struct InventorySlot
{
    uint32_t item_def_id = 0;
    int32_t count = 0;
    bool empty() const { return item_def_id == 0 || count <= 0; }
};

struct InventoryComponent
{
    static constexpr size_t kMaxSlots = 8;
    std::array<InventorySlot, kMaxSlots> slots{};
    int32_t active_slot = -1;

    bool isValidSlot(int32_t slot) const
    {
        return slot >= 0 && slot < static_cast<int32_t>(kMaxSlots);
    }
    const InventorySlot* getSlot(int32_t slot) const
    {
        return isValidSlot(slot) ? &slots[static_cast<size_t>(slot)] : nullptr;
    }
    InventorySlot* getSlot(int32_t slot)
    {
        return isValidSlot(slot) ? &slots[static_cast<size_t>(slot)] : nullptr;
    }
    const InventorySlot* activeSlotData() const
    {
        const InventorySlot* s = getSlot(active_slot);
        return (s && !s->empty()) ? s : nullptr;
    }
    int32_t countOf(uint32_t item_def_id) const
    {
        if (item_def_id == 0)
            return 0;
        int32_t total = 0;
        for (const auto& s : slots)
            if (s.item_def_id == item_def_id && s.count > 0)
                total += s.count;
        return total;
    }
    int32_t findSlot(uint32_t item_def_id) const
    {
        if (item_def_id == 0)
            return -1;
        for (size_t i = 0; i < kMaxSlots; ++i)
            if (slots[i].item_def_id == item_def_id && slots[i].count > 0)
                return static_cast<int32_t>(i);
        return -1;
    }
    int32_t firstEmptySlot() const
    {
        for (size_t i = 0; i < kMaxSlots; ++i)
            if (slots[i].empty())
                return static_cast<int32_t>(i);
        return -1;
    }
    // Stack onto an existing slot for the same def, else first empty slot.
    // Returns false (no mutation) on invalid input or no room.
    bool add(uint32_t item_def_id, int32_t count)
    {
        if (item_def_id == 0 || count <= 0)
            return false;
        int32_t idx = findSlot(item_def_id);
        if (idx < 0)
            idx = firstEmptySlot();
        if (idx < 0)
            return false;
        auto& s = slots[static_cast<size_t>(idx)];
        if (s.empty())
        {
            s.item_def_id = item_def_id;
            s.count = count;
        }
        else
        {
            s.count += count;
        }
        if (active_slot < 0)
            active_slot = idx;
        return true;
    }
    bool addItem(uint32_t item_def_id, int32_t count) { return add(item_def_id, count); }

    // Remove `count` from a specific slot. Returns amount removed.
    // Emptying the active slot clears the selection.
    int32_t removeFromSlot(int32_t slot, int32_t count)
    {
        if (!isValidSlot(slot) || count <= 0)
            return 0;
        auto& s = slots[static_cast<size_t>(slot)];
        if (s.empty())
            return 0;
        const int32_t removed = std::min(count, s.count);
        s.count -= removed;
        if (s.count <= 0)
        {
            s.item_def_id = 0;
            s.count = 0;
            if (active_slot == slot)
                active_slot = -1;
        }
        return removed;
    }
    int32_t drop(int32_t slot, int32_t count) { return removeFromSlot(slot, count); }

    // Remove `count` of a def across slots (lowest index first).
    // Returns false with no mutation when the total is insufficient.
    bool remove(uint32_t item_def_id, int32_t count)
    {
        if (item_def_id == 0 || count <= 0)
            return false;
        if (countOf(item_def_id) < count)
            return false;
        int32_t left = count;
        for (size_t i = 0; i < kMaxSlots && left > 0; ++i)
        {
            if (slots[i].item_def_id != item_def_id || slots[i].count <= 0)
                continue;
            left -= removeFromSlot(static_cast<int32_t>(i), left);
        }
        return left == 0;
    }
    bool removeItem(uint32_t item_def_id, int32_t count) { return remove(item_def_id, count); }

    bool clearSlot(int32_t slot)
    {
        if (!isValidSlot(slot))
            return false;
        slots[static_cast<size_t>(slot)] = InventorySlot{};
        if (active_slot == slot)
            active_slot = -1;
        return true;
    }
    // Only occupied, in-range slots may be selected.
    bool select(int32_t slot)
    {
        if (!isValidSlot(slot))
            return false;
        if (slots[static_cast<size_t>(slot)].empty())
            return false;
        active_slot = slot;
        return true;
    }
    bool selectSlot(int32_t slot) { return select(slot); }

    static void reflect(Reflector<InventoryComponent>& r)
    {
        r.display("Inventory").category("Gameplay");
        r.field<&InventoryComponent::active_slot>("active_slot")
            .tooltip("Active slot index (-1 = none)").category("Inventory");
    }
};

// ---------------------------------------------------------------------------
// Health. Damage goes through DamageSystem::applyDamage (single entry).
// ---------------------------------------------------------------------------
struct HealthComponent
{
    float current = 100.0f;
    float max = 100.0f;
    bool alive = true;
    uint64_t invulnerable_until_tick = 0;

    bool isInvulnerable(uint64_t tick) const
    {
        return tick < invulnerable_until_tick;
    }
    void setInvulnerableUntil(uint64_t tick) { invulnerable_until_tick = tick; }
    void reset()
    {
        current = max;
        alive = max > 0.0f;
    }

    static void reflect(Reflector<HealthComponent>& r)
    {
        r.display("Health").category("Gameplay");
        r.field<&HealthComponent::current>("current")
            .visible().category("Health");
        r.field<&HealthComponent::max>("max")
            .tooltip("Maximum health").drag(1.0f).range(0.0f, 1000000.0f).category("Health");
        r.field<&HealthComponent::alive>("alive").visible().category("Health");
        r.field<&HealthComponent::invulnerable_until_tick>("invulnerable_until_tick")
            .visible().tooltip("Invulnerable while current tick < this value").category("Health");
    }
};

struct DamageInfo
{
    float amount = 0.0f;
    EntityRef instigator;
    EntityRef source;
    uint32_t damage_type = 0;
};

struct DamageResult
{
    bool applied = false;
    bool killed = false;
    bool blocked = false;
    float applied_amount = 0.0f;
    float remaining_health = 0.0f;
};

// ---------------------------------------------------------------------------
// Channel: a timed interaction progressed on the fixed tick.
// ---------------------------------------------------------------------------
struct ChannelComponent
{
    float progress = 0.0f;
    float duration = 0.0f;
    bool active = false;
    uint32_t channel_id = 0;

    void start(uint32_t id, float dur)
    {
        channel_id = id;
        duration = (dur > 0.0f && std::isfinite(dur)) ? dur : 0.0f;
        progress = 0.0f;
        active = duration > 0.0f;
        if (!active)
            progress = 0.0f;
    }
    void startChannel(uint32_t id, float dur) { start(id, dur); }
    // Cancel resets progress (callers may emit ChannelCancelledEvent).
    void cancel()
    {
        active = false;
        progress = 0.0f;
    }
    void reset()
    {
        active = false;
        progress = 0.0f;
        duration = 0.0f;
        channel_id = 0;
    }
    // Returns true exactly on the completing tick.
    bool tick(float dt)
    {
        if (!active || !(dt > 0.0f) || !std::isfinite(dt))
            return false;
        progress += dt;
        if (progress + 1e-6f >= duration)
        {
            progress = duration;
            active = false;
            return true;
        }
        return false;
    }
    float fraction() const
    {
        return duration > 0.0f ? std::clamp(progress / duration, 0.0f, 1.0f) : 0.0f;
    }

    static void reflect(Reflector<ChannelComponent>& r)
    {
        r.display("Channel").category("Gameplay");
        r.field<&ChannelComponent::progress>("progress").visible().category("Channel");
        r.field<&ChannelComponent::duration>("duration")
            .tooltip("Channel duration in seconds").drag(0.01f).range(0.0f, 100000.0f).category("Channel");
        r.field<&ChannelComponent::active>("active").visible().category("Channel");
        r.field<&ChannelComponent::channel_id>("channel_id")
            .tooltip("Opaque channel identifier (game-defined)").category("Channel");
    }
};
