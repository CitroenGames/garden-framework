#pragma once

#include "Components/HierarchyComponents.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
#include "Components/Components.hpp"
#include "EngineExport.h"
#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>

struct PendingDestroy {};

// GUID -> entity index (entity_lifecycle.md §2). Stored in registry.ctx.
struct EntityIndex
{
    std::unordered_map<EntityGuid, entt::entity, GuidHash> by_guid;

    entt::entity resolve(EntityRef ref) const
    {
        if (ref.isNull())
            return entt::null;
        auto it = by_guid.find(ref.guid);
        return it != by_guid.end() ? it->second : entt::null;
    }
};

// Thin lifecycle service over a registry (entity_lifecycle.md §3).
// One per registry; maintains EntityIndex in registry.ctx via Identity hooks.
// Hierarchy destroy policies (hierarchy.md section 6) are applied in
// drainDestroyQueue()/destroyImmediate() via the Hierarchy API.
class EntityLifecycle // header-only (like other component headers); no ENGINE_API
{
public:
    // guid_seed != 0 selects deterministic generation (replay/tests).
    // guid_seed == 0 derives a random startup seed.
    explicit EntityLifecycle(entt::registry& r, uint64_t guid_seed = 0)
        : m_registry(r)
        , m_guid_state(guid_seed != 0 ? guid_seed : randomSeed())
    {
        if (!m_registry.ctx().contains<EntityIndex>())
            m_registry.ctx().emplace<EntityIndex>();
        m_on_construct = m_registry.on_construct<IdentityComponent>()
                             .connect<&EntityLifecycle::onIdentityConstruct>(*this);
        m_on_destroy = m_registry.on_destroy<IdentityComponent>()
                           .connect<&EntityLifecycle::onIdentityDestroy>(*this);
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
        ev.entity = e;
        ev.ref = EntityRef{guid};
        ev.tag = tag_str;
        EventBus::get().queue(std::move(ev)); // deferred per events.md
        return e;
    }

    void destroy(entt::entity e) // deferred
    {
        if (e == entt::null || !m_registry.valid(e))
            return;
        if (m_registry.all_of<PendingDestroy>(e))
            return;
        m_registry.emplace<PendingDestroy>(e);
        m_queue.push_back(e);
    }

    void destroyImmediate(entt::entity e)
    {
        if (e == entt::null || !m_registry.valid(e))
            return;
        publishDestroyed(e);
        if (m_registry.all_of<PendingDestroy>(e))
        {
        }
        applyHierarchyDestroy(e, true);
        if (m_registry.all_of<PendingDestroy>(e))
        {
        if (m_registry.all_of<PendingDestroy>(e))
        {
            for (size_t i = 0; i < m_queue.size(); ++i)
                if (m_queue[i] == e) { m_queue.erase(m_queue.begin() + static_cast<long>(i)); break; }
        }
        }
        m_registry.destroy(e); // on_destroy hook erases index entry
    }

    bool isPendingDestroy(entt::entity e) const
    {
        return e != entt::null && m_registry.valid(e) && m_registry.all_of<PendingDestroy>(e);
    }

    void drainDestroyQueue()
    {
        if (m_draining)
        {
            assert(false && "EntityLifecycle::drainDestroyQueue re-entered");
            return;
        }
        m_draining = true;
        // Loop so CascadeDestroy children enqueued during this drain are
        // also drained in the same pass, FIFO (hierarchy.md section 6).
        while (!m_queue.empty())
        {
        std::vector<entt::entity> queue;
        queue.swap(m_queue);
        for (entt::entity e : queue)
        {
            if (!m_registry.valid(e))
                continue;
            publishDestroyed(e); // BEFORE destroy; handlers may read components
            applyHierarchyDestroy(e, false);
            m_registry.destroy(e);
        }
        } // while (!m_queue.empty())
        m_draining = false;
    }

    size_t pendingCount() const { return m_queue.size(); }

    EntityRef refOf(entt::entity e) const
    {
        if (e == entt::null || !m_registry.valid(e))
            return EntityRef{};
        const auto* id = m_registry.try_get<IdentityComponent>(e);
        return id ? EntityRef{id->guid} : EntityRef{};
    }

    entt::entity resolve(EntityRef ref) const
    {
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
        m_queue.clear();
        index().by_guid.clear();
    }

    template<typename... Ts>
    auto aliveView()
    {
        return m_registry.view<Ts...>(entt::exclude<PendingDestroy>);
    }

private:
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
        } while (index().by_guid.find(g) != index().by_guid.end());
        return g;
    }

    std::string tagOf(entt::entity e) const
    {
        const auto* tag = m_registry.try_get<TagComponent>(e);
        return tag ? tag->name : std::string{};
    }

    void publishDestroyed(entt::entity e)
    {
        EntityDestroyedEvent ev;
        ev.entity = e;
        ev.ref = refOf(e);
        ev.tag = tagOf(e);
        EventBus::get().publish(std::move(ev)); // immediate, before destroy
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

    void onIdentityConstruct(entt::registry& r, entt::entity e)
    {
        const auto& id = r.get<IdentityComponent>(e);
        if (!id.guid.isNull())
            index().by_guid[id.guid] = e;
    }

    void onIdentityDestroy(entt::registry& r, entt::entity e)
    {
        const auto* id = r.try_get<IdentityComponent>(e);
        if (!id)
            return;
        auto& map = index().by_guid;
        auto it = map.find(id->guid);
        if (it != map.end() && it->second == e)
            map.erase(it);
    }

    entt::registry& m_registry;
    uint64_t m_guid_state;
    std::vector<entt::entity> m_queue;
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
