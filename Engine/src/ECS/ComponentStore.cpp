#include "ECS/ComponentStore.hpp"

#include <map>
#include <memory>
#include <new>
#include <tuple>

namespace garden::ecs {
namespace {
using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
Key key(ComponentId id, ComponentEntityKey entity) { return {id.slot, id.generation, entity.slot}; }
bool valid(ComponentEntityKey entity) { return entity.slot != UINT32_MAX && entity.generation != 0; }
ComponentStoreResult catalogResult(ComponentCatalogResult r) {
    if (r == ComponentCatalogResult::ResourceExhausted) return ComponentStoreResult::ResourceExhausted;
    if (r == ComponentCatalogResult::InternalError) return ComponentStoreResult::InternalError;
    return ComponentStoreResult::StaleComponent;
}
}

struct ComponentStore::Impl {
    struct Value {
        ComponentLease lease;
        void* data = nullptr;
        size_t alignment = 0;
        uint32_t generation = 0;
        bool constructed = false;
        ~Value() {
            if (constructed) lease.descriptor()->operations.destroy(data);
            if (data) ::operator delete(data, std::align_val_t(alignment));
        }
    };
    std::map<Key, std::unique_ptr<Value>> values;
};

ComponentStore::ComponentStore() : m_impl(new Impl) {}
ComponentStore::~ComponentStore() { delete m_impl; }

ComponentStoreResult ComponentStore::addErased(ComponentId id, ComponentKey expectedKey, ComponentEntityKey entity,
    size_t size, size_t alignment, Constructor construct, void* context) noexcept {
    if (!valid(entity)) return ComponentStoreResult::InvalidEntity;
    ComponentLease lease;
    auto acquired = ComponentCatalog::instance().acquire(id, ComponentPinKind::LiveInstance, lease);
    if (acquired != ComponentCatalogResult::Success) return catalogResult(acquired);
    auto* descriptor = lease.descriptor();
    if (descriptor->key != expectedKey || descriptor->size != size || descriptor->alignment != alignment)
        return ComponentStoreResult::TypeMismatch;
    for (const auto& [entryKey, entry] : m_impl->values)
        if (std::get<2>(entryKey) == entity.slot && entry->generation != entity.generation)
            return ComponentStoreResult::StaleEntity;
    auto existing = m_impl->values.find(key(id, entity));
    if (existing != m_impl->values.end())
        return existing->second->generation == entity.generation ? ComponentStoreResult::Duplicate : ComponentStoreResult::StaleEntity;
    try {
        auto value = std::make_unique<Impl::Value>();
        value->lease = std::move(lease);
        value->alignment = alignment;
        value->generation = entity.generation;
        value->data = ::operator new(size, std::align_val_t(alignment));
        // Reserve the map node before construction so allocation failure cannot
        // leave a constructed value outside the store.
        auto [it, inserted] = m_impl->values.emplace(key(id, entity), std::move(value));
        if (!construct(it->second->data, context)) {
            m_impl->values.erase(it);
            return ComponentStoreResult::ConstructionFailed;
        }
        it->second->constructed = true;
        return ComponentStoreResult::Success;
    } catch (...) { return ComponentStoreResult::ResourceExhausted; }
}

ComponentStoreResult ComponentStore::get(ComponentId id, ComponentEntityKey entity, void*& out) const noexcept {
    out = nullptr;
    if (!valid(entity)) return ComponentStoreResult::InvalidEntity;
    ComponentLease lease;
    auto acquired = ComponentCatalog::instance().acquire(id, ComponentPinKind::Operation, lease);
    if (acquired != ComponentCatalogResult::Success) return catalogResult(acquired);
    auto it = m_impl->values.find(key(id, entity));
    if (it == m_impl->values.end()) return ComponentStoreResult::NotFound;
    if (it->second->generation != entity.generation) return ComponentStoreResult::StaleEntity;
    out = it->second->data;
    return ComponentStoreResult::Success;
}
ComponentStoreResult ComponentStore::snapshotEntities(ComponentId id, std::vector<ComponentEntityKey>& out) const noexcept {
    out.clear();
    ComponentLease lease;
    auto acquired = ComponentCatalog::instance().acquire(id, ComponentPinKind::Operation, lease);
    if (acquired != ComponentCatalogResult::Success) return catalogResult(acquired);
    try {
        auto it = m_impl->values.lower_bound(Key{id.slot, id.generation, 0});
        for (; it != m_impl->values.end(); ++it) {
            const auto& entryKey = it->first;
            if (std::get<0>(entryKey) != id.slot || std::get<1>(entryKey) != id.generation) break;
            out.push_back({std::get<2>(entryKey), it->second->generation});
        }
        return ComponentStoreResult::Success;
    } catch (...) {
        out.clear();
        return ComponentStoreResult::ResourceExhausted;
    }
}
ComponentStoreResult ComponentStore::remove(ComponentId id, ComponentEntityKey entity) noexcept {
    void* ignored = nullptr;
    auto result = get(id, entity, ignored);
    if (result != ComponentStoreResult::Success) return result;
    m_impl->values.erase(key(id, entity));
    return ComponentStoreResult::Success;
}
ComponentStoreResult ComponentStore::removeEntity(ComponentEntityKey entity) noexcept {
    if (!valid(entity)) return ComponentStoreResult::InvalidEntity;
    bool found = false;
    for (const auto& [entryKey, entry] : m_impl->values) {
        if (std::get<2>(entryKey) != entity.slot) continue;
        if (entry->generation != entity.generation) return ComponentStoreResult::StaleEntity;
        found = true;
    }
    if (!found) return ComponentStoreResult::NotFound;
    for (auto it = m_impl->values.begin(); it != m_impl->values.end();) {
        if (std::get<2>(it->first) == entity.slot) it = m_impl->values.erase(it);
        else ++it;
    }
    return ComponentStoreResult::Success;
}
void ComponentStore::clear() noexcept { m_impl->values.clear(); }
} // namespace garden::ecs
