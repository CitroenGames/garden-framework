#pragma once

#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
#include "Components/Components.hpp"
#include "EngineExport.h"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <list>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <entt/entt.hpp>

struct PendingDestroy {};

// GUID -> entity index (entity_lifecycle.md §2). Stored in registry.ctx.
struct EntityIndex
{
    // An entry whose value is entt::null represents an ambiguous GUID.
    // Keep that entry so identity generation still reserves the GUID.
    std::unordered_map<EntityGuid, entt::entity, GuidHash> by_guid;

    entt::entity resolve(EntityRef ref) const
    {
        if (degraded || ref.isNull())
            return entt::null;
        auto it = by_guid.find(ref.guid);
        return it != by_guid.end() ? it->second : entt::null;
    }

private:
    friend class EntityLifecycle;
    // A failed allocation or detected inconsistency invalidates the entire
    // index. This flag requires no allocation and stays set until rebuilt.
    bool degraded = false;
    // Zero is reserved. Clearing advances the shared epoch and skips zero on
    // wrap; a queue must not survive a full 2^64-1 clear cycle.
    uint64_t clear_generation = 1;
    // Only ambiguous identities need a list. Destruction signals can arrive
    // before a bulk storage clear removes any of its components.
    std::unordered_map<EntityGuid, std::vector<entt::entity>, GuidHash> duplicate_owners;
};

// Thin lifecycle service over a registry (entity_lifecycle.md §3).
// Instances share EntityIndex and destruction guards through the registry;
// each instance owns its deferred queue.
// Hierarchy destroy policies (hierarchy.md section 6) are applied in
// drainDestroyQueue()/destroyImmediate() via the Hierarchy API.
class EntityLifecycle // header-only (like other component headers); no ENGINE_API
{
    struct DestroyInProgress {};
    // Survives an exception and is shared by all services on this registry.
    struct DestroyPublicationStarted {};

    // The marker is shared by all lifecycle instances using this registry.
    // Its scope includes publication, hierarchy cleanup, and final retirement.
    class DestructionGuard
    {
    public:
        DestructionGuard(entt::registry& registry, entt::entity entity)
            : m_registry(registry), m_entity(entity)
        {
            m_registry.emplace<DestroyInProgress>(m_entity);
        }
        ~DestructionGuard()
        {
            if (m_registry.valid(m_entity) && m_registry.all_of<DestroyInProgress>(m_entity))
                m_registry.remove<DestroyInProgress>(m_entity);
        }
        DestructionGuard(const DestructionGuard&) = delete;
        DestructionGuard& operator=(const DestructionGuard&) = delete;

    private:
        entt::registry& m_registry;
        entt::entity m_entity;
    };

public:
    // guid_seed != 0 selects deterministic generation (replay/tests).
    // guid_seed == 0 derives a random startup seed.
    explicit EntityLifecycle(entt::registry& r, uint64_t guid_seed = 0)
        : m_registry(r)
        , m_guid_state(guid_seed != 0 ? guid_seed : randomSeed())
    {
        if (!m_registry.ctx().contains<EntityIndex>())
            m_registry.ctx().emplace<EntityIndex>();
        m_queue_generation = index().clear_generation;
        m_on_construct = m_registry.on_construct<IdentityComponent>()
                             .connect<&EntityLifecycle::onIdentityConstruct>(*this);
        m_on_destroy = m_registry.on_destroy<IdentityComponent>()
                           .connect<&EntityLifecycle::onIdentityDestroy>(*this);
        // Registries cloned before this service is created (notably Editor
        // PIE worlds) already contain identities, so seed the index after
        // installing hooks for subsequent component changes.
        index().by_guid.clear();
        index().duplicate_owners.clear();
        index().degraded = false;
        for (auto [e, identity] : m_registry.view<IdentityComponent>().each())
            if (!identity.guid.isNull())
                recordIdentity(identity.guid, e);
    }

    ~EntityLifecycle()
    {
        m_on_construct.release();
        m_on_destroy.release();
    }

    EntityLifecycle(const EntityLifecycle&) = delete;
    EntityLifecycle& operator=(const EntityLifecycle&) = delete;

    entt::entity create(std::string_view tag = "")
    {
        entt::entity e = m_registry.create();
        EntityGuid guid = nextGuid();
        m_registry.emplace<IdentityComponent>(e, guid); // hook inserts into index
        std::string tag_str(tag);
        if (!tag.empty())
            m_registry.emplace_or_replace<TagComponent>(e, TagComponent{tag_str});
        EntityCreatedEvent ev;
        ev.ref = EntityRef{guid};
        ev.tag = tag_str;
        EventBus::get().queue(std::move(ev)); // deferred per events.md
        return e;
    }

    void destroy(entt::entity e) // deferred
    {
        discardStaleQueue();
        if (e == entt::null || !m_registry.valid(e))
            return;
        if (m_registry.all_of<PendingDestroy>(e))
            return;
        m_registry.emplace<PendingDestroy>(e);
        try
        {
            m_queue.push_back(e);
        }
        catch (...)
        {
            m_registry.remove<PendingDestroy>(e);
            throw;
        }
    }

    void destroyImmediate(entt::entity e)
    {
        discardStaleQueue();
        if (e == entt::null || !m_registry.valid(e) || m_registry.all_of<DestroyInProgress>(e))
            return;
        DestructionGuard guard(m_registry, e);
        try
        {
            publishDestroyedOnce(e);
            if (m_registry.valid(e))
                applyHierarchyDestroy(e, true);
            if (m_registry.valid(e))
                m_registry.destroy(e); // on_destroy hook erases index entry
            removeQueuedEntity(e);
        }
        catch (...)
        {
            // A previously queued request remains in its original position.
            // For an unqueued entity, make a later drain resume retirement.
            if (m_registry.valid(e) && !m_registry.all_of<PendingDestroy>(e))
                destroy(e);
            throw;
        }
    }

    bool isPendingDestroy(entt::entity e) const
    {
        return e != entt::null && m_registry.valid(e) && m_registry.all_of<PendingDestroy>(e);
    }

    void drainDestroyQueue()
    {
        discardStaleQueue();
        if (m_draining)
        {
            assert(false && "EntityLifecycle::drainDestroyQueue re-entered");
            return;
        }
        m_draining = true;
        struct DrainGuard
        {
            bool& draining;
            ~DrainGuard() { draining = false; }
        } drain_guard{m_draining};
        const uint64_t drain_generation = m_queue_generation;
        // Splicing restores an interrupted batch ahead of newly queued work
        // without allocating while an exception is already in flight.
        while (!m_queue.empty() && drain_generation == index().clear_generation)
        {
            std::list<entt::entity> queue;
            queue.splice(queue.end(), m_queue);
            try
            {
                while (!queue.empty() && drain_generation == index().clear_generation)
                {
                    const entt::entity e = queue.front();
                    if (!m_registry.valid(e) || !m_registry.all_of<PendingDestroy>(e))
                    {
                        queue.pop_front();
                        continue;
                    }
                    if (m_registry.all_of<DestroyInProgress>(e))
                    {
                        // A peer is destroying this entity. Keep this request
                        // and its successors in FIFO order for a later pass.
                        m_queue.splice(m_queue.begin(), queue);
                        return;
                    }
                    DestructionGuard guard(m_registry, e);
                    publishDestroyedOnce(e);
                    if (m_registry.valid(e))
                        applyHierarchyDestroy(e, false);
                    if (m_registry.valid(e))
                        m_registry.destroy(e);
                    queue.pop_front();
                }
            }
            catch (...)
            {
                if (drain_generation == index().clear_generation)
                    m_queue.splice(m_queue.begin(), queue);
                else if (!queue.empty())
                {
                    // clear() cancels the remainder, but publication commits
                    // the current entity even when a later observer throws.
                    const entt::entity current = queue.front();
                    if (m_registry.valid(current) &&
                        m_registry.all_of<DestroyPublicationStarted>(current) &&
                        !m_registry.all_of<PendingDestroy>(current))
                    {
                        discardStaleQueue();
                        m_registry.emplace<PendingDestroy>(current);
                        m_queue.splice(m_queue.begin(), queue, queue.begin());
                    }
                }
                throw;
            }
        }
    }

    size_t pendingCount() const
    {
        return m_queue_generation == index().clear_generation ? m_queue.size() : 0;
    }

    EntityRef refOf(entt::entity e) const
    {
        if (e == entt::null || !m_registry.valid(e))
            return EntityRef{};
        const auto* id = m_registry.try_get<IdentityComponent>(e);
        return id ? EntityRef{id->guid} : EntityRef{};
    }

    entt::entity resolve(EntityRef ref) const
    {
        if (ref.isNull())
            return entt::null;
        if (index().degraded)
        {
            // The const registry view does not create component storage or
            // allocate. Reject an ambiguous GUID even in a damaged index.
            entt::entity found = entt::null;
            for (auto [candidate, identity] : std::as_const(m_registry).view<IdentityComponent>().each())
            {
                if (identity.guid != ref.guid)
                    continue;
                if (found != entt::null)
                    return entt::null;
                found = candidate;
            }
            return found;
        }
        entt::entity e = index().resolve(ref);
        if (e != entt::null && !m_registry.valid(e))
            return entt::null;
        // Guard against recycled id whose Identity no longer matches.
        if (e != entt::null)
        {
            const auto* id = m_registry.try_get<IdentityComponent>(e);
            if (!id || id->guid != ref.guid)
                return entt::null;
        }
        return e;
    }

    EntityIndex& index() { return m_registry.ctx().get<EntityIndex>(); }
    const EntityIndex& index() const { return m_registry.ctx().get<EntityIndex>(); }

    void clear()
    {
        auto& generation = index().clear_generation;
        if (++generation == 0)
            ++generation;
        m_queue.clear();
        m_queue_generation = generation;
        // Cancelling the queue must also make live entities eligible for
        // aliveView and a later destroy request. Avoid creating an empty pool.
        if (std::as_const(m_registry).storage<PendingDestroy>())
            m_registry.clear<PendingDestroy>();
        index().by_guid.clear();
        index().duplicate_owners.clear();
        index().degraded = false;
        // clear() can be called before the registry is cleared. Rebuild from
        // its live identities rather than allowing new GUIDs to collide.
        for (auto [e, identity] : std::as_const(m_registry).view<IdentityComponent>().each())
            if (!identity.guid.isNull())
                recordIdentity(identity.guid, e);
    }

    template<typename... Ts>
    auto aliveView()
    {
        return m_registry.view<Ts...>(entt::exclude<PendingDestroy>);
    }

private:
    void discardStaleQueue()
    {
        if (m_queue_generation != index().clear_generation)
        {
            m_queue.clear();
            m_queue_generation = index().clear_generation;
        }
    }

    void removeQueuedEntity(entt::entity e)
    {
        discardStaleQueue();
        m_queue.remove(e);
    }

    static uint64_t randomSeed()
    {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        s ^= static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return s != 0 ? s : 0x9e3779b97f4a7c15ull;
    }

    EntityGuid nextGuid()
    {
        EntityGuid g;
        do
        {
            g.hi = splitmix64Next(m_guid_state);
            g.lo = splitmix64Next(m_guid_state);
            if (g.isNull())
                g.lo = 1;
        } while (guidExists(g));
        return g;
    }

    bool guidExists(EntityGuid guid) const
    {
        if (!index().degraded)
            return index().by_guid.find(guid) != index().by_guid.end();
        for (auto [entity, identity] : std::as_const(m_registry).view<IdentityComponent>().each())
        {
            (void)entity;
            if (identity.guid == guid)
                return true;
        }
        return false;
    }

    std::string tagOf(entt::entity e) const
    {
        const auto* tag = m_registry.try_get<TagComponent>(e);
        return tag ? tag->name : std::string{};
    }

    void publishDestroyedOnce(entt::entity e)
    {
        if (m_registry.all_of<DestroyPublicationStarted>(e))
            return;
        EntityDestroyedEvent ev;
        ev.ref = refOf(e);
        ev.tag = tagOf(e);
        // Prepare the dispatcher before committing publication so allocation
        // failures before dispatch leave the event eligible for retry.
        auto& bus = EventBus::get();
        (void)bus.sink<EntityDestroyedEvent>();
        // A callback can throw after other observers have seen this event.
        // Keep the marker until retirement so no lifecycle republishes it.
        m_registry.emplace<DestroyPublicationStarted>(e);
        bus.publish(std::move(ev)); // immediate, before destroy
    }

    // hierarchy.md section 6: apply E's destroy policy to its children,
    // then unlink E from its own parent's Children list.
    void applyHierarchyDestroy(entt::entity e, bool immediate)
    {
        const auto* children_comp = m_registry.try_get<ChildrenComponent>(e);
        const std::vector<EntityRef> snapshot =
            children_comp ? children_comp->children : std::vector<EntityRef>{};
        HierarchyDestroyPolicy policy = HierarchyDestroyPolicy::OrphanToParent;
        if (const auto* p = m_registry.try_get<HierarchyPolicyComponent>(e))
            policy = p->on_parent_destroyed;
        const entt::entity grandparent = Hierarchy::parentOf(m_registry, e);
        for (EntityRef child_ref : snapshot)
        {
            entt::entity child = resolve(child_ref);
            if (child == entt::null || !m_registry.valid(child))
                continue;
            if (isPendingDestroy(child))
                continue; // already queued; orphan step skips it (§6)
            switch (policy)
            {
            case HierarchyDestroyPolicy::CascadeDestroy: {
                const auto descendants =
                    Hierarchy::collectDescendants(m_registry, child);
                if (immediate)
                {
                    destroyImmediate(child);
                    for (entt::entity d : descendants) destroyImmediate(d);
                }
                else
                {
                    destroy(child);
                    for (entt::entity d : descendants) destroy(d);
                }
                break; }
                // Whole subtree is destroyed (H6): descendants are queued
                // too, not just immediate children. They are collected in
                // the loop below via collectDescendants on first child;
                // per-child handling here only queues the child itself.
                break;
            case HierarchyDestroyPolicy::COUNT:
                if (immediate)
                    destroyImmediate(child);
                    /* whole subtree already handled above */
                else
                    destroy(child); // deferred; drained same pass, FIFO
                break;
            case HierarchyDestroyPolicy::ReparentToRoot:
                Hierarchy::detach(m_registry, child, true);
                break;
            case HierarchyDestroyPolicy::OrphanToParent:
            default:
                if (grandparent != entt::null)
                    Hierarchy::attach(m_registry, child, grandparent, true);
                else
                    Hierarchy::detach(m_registry, child, true);
                break;
            }
        }
        Hierarchy::unlinkFromParentList(m_registry, e);
    }

    void indexIdentity(EntityGuid guid, entt::entity e)
    {
        auto& entity_index = index();
        auto [it, inserted] = entity_index.by_guid.try_emplace(guid, e);
        if (inserted || it->second == e)
            return;
        if (it->second == entt::null)
        {
            auto owners = entity_index.duplicate_owners.find(guid);
            if (owners == entity_index.duplicate_owners.end())
            {
                entity_index.degraded = true;
                return;
            }
            if (std::find(owners->second.begin(), owners->second.end(), e) == owners->second.end())
                owners->second.push_back(e);
            return;
        }

        // Build the owner list before marking the lookup ambiguous. If an
        // allocation fails, recordIdentity marks the index degraded.
        std::vector<entt::entity> owners{it->second, e};
        if (!entity_index.duplicate_owners.emplace(guid, std::move(owners)).second)
        {
            entity_index.degraded = true;
            return;
        }
        it->second = entt::null;
    }

    void recordIdentity(EntityGuid guid, entt::entity e) noexcept
    {
        auto& entity_index = index();
        if (entity_index.degraded)
            return;
        try
        {
            indexIdentity(guid, e);
        }
        catch (...)
        {
            // The component is already present when EnTT invokes this hook.
            // Never trust a partially updated index after allocation failure.
            entity_index.degraded = true;
        }
    }

    void onIdentityConstruct(entt::registry& r, entt::entity e)
    {
        const auto& id = r.get<IdentityComponent>(e);
        if (!id.guid.isNull())
            recordIdentity(id.guid, e);
    }

    void onIdentityDestroy(entt::registry& r, entt::entity e)
    {
        const auto* id = r.try_get<IdentityComponent>(e);
        if (!id || id->guid.isNull() || index().degraded)
            return;
        auto& map = index().by_guid;
        auto it = map.find(id->guid);
        if (it == map.end())
            return; // Another lifecycle subscriber may already have retired it.
        if (it->second == e)
        {
            map.erase(it);
            return;
        }
        if (it->second != entt::null)
            return; // Another subscriber may already have restored this owner.

        auto& duplicates = index().duplicate_owners;
        auto owners_it = duplicates.find(id->guid);
        if (owners_it == duplicates.end())
        {
            index().degraded = true;
            return;
        }
        auto& owners = owners_it->second;
        const auto owner_it = std::find(owners.begin(), owners.end(), e);
        if (owner_it == owners.end())
            return; // This owner may already have been removed by a subscriber.
        owners.erase(owner_it);
        if (owners.size() == 1)
        {
            it->second = owners.front();
            duplicates.erase(owners_it);
        }
        else if (owners.empty())
        {
            map.erase(it);
            duplicates.erase(owners_it);
        }
    }

    entt::registry& m_registry;
    uint64_t m_guid_state;
    std::list<entt::entity> m_queue;
    uint64_t m_queue_generation = 0;
    bool m_draining = false;
    entt::scoped_connection m_on_construct;
    entt::scoped_connection m_on_destroy;
};

// Free alive-view helper (entity_lifecycle.md §4 rule 2).
template<typename... Ts>
auto aliveView(entt::registry& r)
{
    return r.view<Ts...>(entt::exclude<PendingDestroy>);
}
