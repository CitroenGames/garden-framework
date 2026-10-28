#include "ECS/ComponentCatalog.hpp"

#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace garden::ecs {
namespace {

// Reject malformed UTF-8, overlong sequences, surrogates and non-scalar values.
bool validName(std::string_view name) noexcept {
    if (name.empty() || name.front() == '.' || name.back() == '.') return false;
    bool hasSeparator = false;
    bool previousSeparator = false;
    for (size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            if (c <= 0x20 || c == 0x7f || (c == '.' && previousSeparator)) return false;
            previousSeparator = c == '.';
            hasSeparator |= previousSeparator;
            ++i;
            continue;
        }
        previousSeparator = false;
        unsigned count = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 2; value = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; value = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; value = c & 0x07; minimum = 0x10000; }
        else return false;
        if (name.size() - i < count) return false;
        for (unsigned j = 1; j < count; ++j) {
            const auto next = static_cast<unsigned char>(name[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        i += count;
    }
    return hasSeparator;
}

bool validOwner(const ComponentOwnerId& owner) noexcept {
    bool hasDigest = false;
    for (const auto byte : owner.imageSha256) hasDigest |= byte != 0;
    if (owner.kind == ComponentOwnerKind::EngineCore)
        return !hasDigest && owner.apiAbiVersion == 0;
    if (owner.kind == ComponentOwnerKind::Module)
        return hasDigest && owner.apiAbiVersion != 0;
    return false;
}

bool validTypeMetadata(ComponentKey key, size_t size, size_t alignment, uint32_t capabilities) noexcept {
    constexpr uint32_t known = ComponentDefaultConstruct | ComponentCopyConstruct | ComponentSerialize;
    return validName(key.name) && key.revision != 0 && size != 0 && alignment != 0 &&
        (alignment & (alignment - 1)) == 0 && size % alignment == 0 && (capabilities & ~known) == 0;
}

bool validMetadata(const ComponentDescriptor& d) noexcept {
    return validOwner(d.ownerId) && validTypeMetadata(d.key, d.size, d.alignment, d.capabilities);
}

bool validDescriptor(const ComponentDescriptor& d) noexcept {
    return validMetadata(d) && d.operations.destroy &&
        bool(d.operations.defaultConstruct) == bool(d.capabilities & ComponentDefaultConstruct) &&
        bool(d.operations.copyConstruct) == bool(d.capabilities & ComponentCopyConstruct) &&
        bool(d.operations.serialize) == bool(d.capabilities & ComponentSerialize);
}

bool sameMetadata(const ComponentDescriptor& a, const ComponentDescriptor& b) noexcept {
    return a.key == b.key && a.ownerId == b.ownerId && a.size == b.size &&
        a.alignment == b.alignment && a.capabilities == b.capabilities;
}
}

bool isValidComponentDescriptor(const ComponentDescriptor& descriptor) noexcept {
    return validDescriptor(descriptor);
}

struct ComponentCatalog::Impl {
    struct Entry {
        std::string name;
        ComponentDescriptor descriptor;
        uint32_t generation = 1;
        uint64_t operations = 0;
        uint64_t instances = 0;
        bool active = false;
    };
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<ComponentOwnerId> retiring;
};

ComponentCatalog::ComponentCatalog(Impl* impl) noexcept : m_impl(impl) {}
ComponentCatalog::~ComponentCatalog() = default;

ComponentCatalog& ComponentCatalog::instance() noexcept {
    // Static storage avoids a fallible singleton allocation. Intentionally never
    // destroy these objects: leases in other static destructors can still release.
    alignas(Impl) static unsigned char implStorage[sizeof(Impl)];
    alignas(ComponentCatalog) static unsigned char catalogStorage[sizeof(ComponentCatalog)];
    static ComponentCatalog* const catalog = []() noexcept {
        auto* impl = ::new (implStorage) Impl;
        return ::new (catalogStorage) ComponentCatalog(impl);
    }();
    return *catalog;
}

ComponentCatalogResult ComponentCatalog::registerComponent(const ComponentDescriptor& d, ComponentId& outId) noexcept {
    outId = {};
    if (!validDescriptor(d)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (const auto& owner : m_impl->retiring) if (owner == d.ownerId) return ComponentCatalogResult::InUse;
        size_t freeSlot = m_impl->entries.size();
        for (size_t i = 0; i < m_impl->entries.size(); ++i) {
            const auto& e = *m_impl->entries[i];
            if (e.active && e.name == d.key.name) {
                if (!sameMetadata(e.descriptor, d)) return ComponentCatalogResult::Conflict;
                outId = {static_cast<uint32_t>(i), e.generation};
                return ComponentCatalogResult::DuplicateKey;
            }
            if (!e.active && e.generation != UINT32_MAX && freeSlot == m_impl->entries.size()) freeSlot = i;
        }
        if (freeSlot >= UINT32_MAX) return ComponentCatalogResult::ResourceExhausted;
        // Complete all fallible work before changing an existing slot or publishing
        // a new one. The final swap and descriptor assignments cannot throw.
        std::string name(d.key.name);
        if (freeSlot == m_impl->entries.size()) {
            auto entry = std::make_unique<Impl::Entry>();
            entry->name.swap(name);
            entry->descriptor = d;
            entry->descriptor.key.name = entry->name;
            entry->active = true;
            m_impl->entries.push_back(std::move(entry));
        } else {
            auto& entry = *m_impl->entries[freeSlot];
            entry.name.swap(name);
            entry.descriptor = d;
            entry.descriptor.key.name = entry.name;
            entry.active = true;
        }
        outId = {static_cast<uint32_t>(freeSlot), m_impl->entries[freeSlot]->generation};
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::registerBatch(ComponentOwnerId owner,
    std::span<const ComponentDescriptor> descriptors, std::span<ComponentId> outIds) noexcept {
    for (auto& id : outIds) id = {};
    if (!validOwner(owner) || outIds.size() != descriptors.size())
        return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (const auto& blocked : m_impl->retiring) if (blocked == owner) return ComponentCatalogResult::InUse;
        struct Staged { size_t slot; std::unique_ptr<Impl::Entry> entry; };
        std::vector<Staged> staged;
        std::vector<ComponentId> ids(descriptors.size());
        staged.reserve(descriptors.size());
        size_t nextFree = 0;
        size_t appendCount = 0;
        for (size_t i = 0; i < descriptors.size(); ++i) {
            auto d = descriptors[i];
            d.ownerId = owner;
            if (!validDescriptor(d)) return ComponentCatalogResult::InvalidDescriptor;
            for (size_t j = 0; j < i; ++j)
                if (descriptors[j].key.name == d.key.name) return ComponentCatalogResult::Conflict;
            bool existing = false;
            for (size_t slot = 0; slot < m_impl->entries.size(); ++slot) {
                const auto& entry = *m_impl->entries[slot];
                if (entry.active && entry.name == d.key.name) {
                    if (!sameMetadata(entry.descriptor, d)) return ComponentCatalogResult::Conflict;
                    ids[i] = {static_cast<uint32_t>(slot), entry.generation};
                    existing = true;
                    break;
                }
            }
            if (existing) continue;
            while (nextFree < m_impl->entries.size() &&
                (m_impl->entries[nextFree]->active || m_impl->entries[nextFree]->generation == UINT32_MAX))
                ++nextFree;
            const size_t slot = nextFree < m_impl->entries.size() ? nextFree++ : m_impl->entries.size() + appendCount++;
            if (slot >= UINT32_MAX) return ComponentCatalogResult::ResourceExhausted;
            auto entry = std::make_unique<Impl::Entry>();
            entry->name = d.key.name;
            entry->descriptor = d;
            entry->descriptor.key.name = entry->name;
            if (slot < m_impl->entries.size()) entry->generation = m_impl->entries[slot]->generation;
            entry->active = true;
            ids[i] = {static_cast<uint32_t>(slot), entry->generation};
            staged.push_back({slot, std::move(entry)});
        }
        // No operation after reserve can allocate or throw. Inactive slots have
        // no leases, so replacing their entries preserves all live descriptors.
        m_impl->entries.reserve(m_impl->entries.size() + appendCount);
        for (auto& item : staged) {
            if (item.slot < m_impl->entries.size()) m_impl->entries[item.slot].swap(item.entry);
            else m_impl->entries.push_back(std::move(item.entry));
        }
        for (size_t i = 0; i < ids.size(); ++i) outIds[i] = ids[i];
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::resolve(const ComponentDescriptor& d, ComponentId& outId) const noexcept {
    outId = {};
    if (!validMetadata(d)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (size_t i = 0; i < m_impl->entries.size(); ++i) {
            const auto& e = *m_impl->entries[i];
            if (e.active && e.name == d.key.name) {
                if (!sameMetadata(e.descriptor, d)) return ComponentCatalogResult::Conflict;
                outId = {static_cast<uint32_t>(i), e.generation};
                return ComponentCatalogResult::Success;
            }
        }
        return ComponentCatalogResult::NotFound;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::resolveType(ComponentKey key, size_t size, size_t alignment,
    uint32_t capabilities, ComponentId& outId) const noexcept {
    outId = {};
    if (!validTypeMetadata(key, size, alignment, capabilities)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (size_t i = 0; i < m_impl->entries.size(); ++i) {
            const auto& e = *m_impl->entries[i];
            if (e.active && e.name == key.name) {
                const auto& d = e.descriptor;
                if (d.key != key || d.size != size || d.alignment != alignment || d.capabilities != capabilities)
                    return ComponentCatalogResult::Conflict;
                outId = {static_cast<uint32_t>(i), e.generation};
                return ComponentCatalogResult::Success;
            }
        }
        return ComponentCatalogResult::NotFound;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::beginRetirement(ComponentOwnerId owner) noexcept {
    if (!validOwner(owner)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        for (const auto& blocked : m_impl->retiring)
            if (blocked == owner) return ComponentCatalogResult::Success;
        m_impl->retiring.push_back(owner);
        return ComponentCatalogResult::Success;
    } catch (...) { return ComponentCatalogResult::ResourceExhausted; }
}

ComponentCatalogResult ComponentCatalog::reopenOwner(ComponentOwnerId owner) noexcept {
    try {
        std::lock_guard lock(m_impl->mutex);
        for (auto it = m_impl->retiring.begin(); it != m_impl->retiring.end(); ++it)
            if (*it == owner) { m_impl->retiring.erase(it); break; }
        return ComponentCatalogResult::Success;
    } catch (...) { return ComponentCatalogResult::InternalError; }
}

ComponentCatalogResult ComponentCatalog::unregisterOwner(ComponentOwnerId ownerId) noexcept {
    if (!validOwner(ownerId)) return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        bool found = false;
        for (const auto& entry : m_impl->entries) {
            const auto& e = *entry;
            if (!e.active || e.descriptor.ownerId != ownerId) continue;
            found = true;
            if (e.operations || e.instances) return ComponentCatalogResult::InUse;
        }
        if (!found) return ComponentCatalogResult::NotFound;
        for (auto& entry : m_impl->entries) {
            auto& e = *entry;
            if (!e.active || e.descriptor.ownerId != ownerId) continue;
            e.active = false;
            e.descriptor = {};
            e.name.clear();
            if (e.generation != UINT32_MAX) ++e.generation;
        }
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

ComponentCatalogResult ComponentCatalog::acquire(ComponentId id, ComponentPinKind kind, ComponentLease& outLease) noexcept {
    if (kind != ComponentPinKind::Operation && kind != ComponentPinKind::LiveInstance)
        return ComponentCatalogResult::InvalidDescriptor;
    try {
        std::lock_guard lock(m_impl->mutex);
        if (id.slot >= m_impl->entries.size()) return ComponentCatalogResult::StaleId;
        auto& e = *m_impl->entries[id.slot];
        if (!e.active || e.generation != id.generation) return ComponentCatalogResult::StaleId;
        auto& count = kind == ComponentPinKind::Operation ? e.operations : e.instances;
        if (count == std::numeric_limits<uint64_t>::max()) return ComponentCatalogResult::ResourceExhausted;
        ++count;
        if (outLease.m_catalog) {
            // All leases come from the one process catalog; update both counts
            // while holding the same lock, after the replacement is validated.
            auto& previous = *m_impl->entries[outLease.m_id.slot];
            auto& previousCount = outLease.m_kind == ComponentPinKind::Operation
                ? previous.operations : previous.instances;
            --previousCount;
        }
        outLease.m_catalog = this;
        outLease.m_descriptor = &e.descriptor;
        outLease.m_id = id;
        outLease.m_kind = kind;
        return ComponentCatalogResult::Success;
    } catch (const std::bad_alloc&) {
        return ComponentCatalogResult::ResourceExhausted;
    } catch (...) {
        return ComponentCatalogResult::InternalError;
    }
}

void ComponentCatalog::release(ComponentId id, ComponentPinKind kind) noexcept {
    try {
        std::lock_guard lock(m_impl->mutex);
        auto& e = *m_impl->entries[id.slot];
        auto& count = kind == ComponentPinKind::Operation ? e.operations : e.instances;
        --count;
    } catch (...) {
        // Preserve the pin if locking fails; retirement must remain blocked.
    }
}

ComponentLease::~ComponentLease() { reset(); }
ComponentLease::ComponentLease(ComponentLease&& other) noexcept
    : m_catalog(other.m_catalog), m_descriptor(other.m_descriptor), m_id(other.m_id), m_kind(other.m_kind) {
    other.m_catalog = nullptr;
    other.m_descriptor = nullptr;
}
ComponentLease& ComponentLease::operator=(ComponentLease&& other) noexcept {
    if (this != &other) {
        reset();
        m_catalog = other.m_catalog;
        m_descriptor = other.m_descriptor;
        m_id = other.m_id;
        m_kind = other.m_kind;
        other.m_catalog = nullptr;
        other.m_descriptor = nullptr;
    }
    return *this;
}
void ComponentLease::reset() noexcept {
    if (m_catalog) m_catalog->release(m_id, m_kind);
    m_catalog = nullptr;
    m_descriptor = nullptr;
}

} // namespace garden::ecs
